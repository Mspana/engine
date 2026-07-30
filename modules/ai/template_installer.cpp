// modules/ai/template_installer.cpp
// See template_installer.h. Machinery ported from AgenticOrchestrator's
// async install_export_templates state machine (7/30).

#include "template_installer.h"

#include "actions/export_actions.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/zip_io.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/string/print_string.h"
#include "editor/editor_node.h"
#include "editor/editor_paths.h"
#include "scene/main/http_request.h"
#include "scene/main/scene_tree.h"

// Heap context shared between the installer (main thread) and the extraction
// worker task. Refcounted (2 owners at spawn) so cancel never has to block
// waiting for the worker: whichever side lets go last frees it.
struct AITplExtractContext {
	String tpz_path;
	String dest_dir;
	SafeNumeric<int32_t> files_done;
	SafeNumeric<int32_t> files_total;
	SafeNumeric<int64_t> bytes_written;
	SafeFlag cancel;
	SafeFlag finished; // Set by the task AFTER `error` is final.
	String error; // Empty = success. Only read after `finished` is set.
	SafeNumeric<int32_t> refs;
};

static void _ai_tpl_release_ctx(AITplExtractContext *p_ctx) {
	if (p_ctx->refs.decrement() == 0) {
		memdelete(p_ctx);
	}
}

// Runs on a WorkerThreadPool thread. Extracts the .tpz (a zip whose entries
// all live under a single top-level "templates/" dir — the first path segment
// is stripped) into dest_dir.
static void _ai_tpl_extract_task(void *p_userdata) {
	AITplExtractContext *ctx = (AITplExtractContext *)p_userdata;

	Ref<FileAccess> io_fa;
	zlib_filefunc_def io = zipio_create_io(&io_fa);
	unzFile pkg = unzOpen2(ctx->tpz_path.utf8().get_data(), &io);
	if (!pkg) {
		ctx->error = "Could not open the downloaded template package (corrupt download?).";
		ctx->finished.set();
		_ai_tpl_release_ctx(ctx);
		return;
	}

	// First pass: count extractable files for progress reporting.
	int total = 0;
	int ret = unzGoToFirstFile(pkg);
	while (ret == UNZ_OK) {
		unz_file_info64 info;
		String source_name;
		if (godot_unzip_get_current_file_info(pkg, info, source_name) == UNZ_OK) {
			int slash = source_name.find("/");
			if (!source_name.ends_with("/") && !source_name.begins_with("__MACOSX") &&
					slash >= 0 && slash + 1 < source_name.length()) {
				total++;
			}
		}
		ret = unzGoToNextFile(pkg);
	}
	ctx->files_total.set(total);
	if (total == 0) {
		unzClose(pkg);
		ctx->error = "The template package contains no files in the expected layout.";
		ctx->finished.set();
		_ai_tpl_release_ctx(ctx);
		return;
	}

	// Second pass: extract.
	ret = unzGoToFirstFile(pkg);
	while (ret == UNZ_OK) {
		if (ctx->cancel.is_set()) {
			break;
		}
		unz_file_info64 info;
		String source_name;
		if (godot_unzip_get_current_file_info(pkg, info, source_name) != UNZ_OK) {
			ret = unzGoToNextFile(pkg);
			continue;
		}
		int slash = source_name.find("/");
		if (source_name.ends_with("/") || source_name.begins_with("__MACOSX") ||
				slash < 0 || slash + 1 >= source_name.length()) {
			ret = unzGoToNextFile(pkg);
			continue;
		}
		String rel = source_name.substr(slash + 1);
		String dest_path = ctx->dest_dir.path_join(rel);

		Error mk = DirAccess::make_dir_recursive_absolute(dest_path.get_base_dir());
		if (mk != OK && mk != ERR_ALREADY_EXISTS) {
			ctx->error = vformat("Could not create directory '%s'.", dest_path.get_base_dir());
			break;
		}

		Vector<uint8_t> data;
		data.resize(info.uncompressed_size);
		unzOpenCurrentFile(pkg);
		int64_t read = data.is_empty() ? 0 : (int64_t)unzReadCurrentFile(pkg, data.ptrw(), data.size());
		unzCloseCurrentFile(pkg);
		if (read != (int64_t)data.size()) {
			ctx->error = vformat("Failed to read '%s' from the template package.", source_name);
			break;
		}

		Ref<FileAccess> out = FileAccess::open(dest_path, FileAccess::WRITE);
		if (out.is_null()) {
			ctx->error = vformat("Could not write '%s'.", dest_path);
			break;
		}
		if (!data.is_empty()) {
			out->store_buffer(data.ptr(), data.size());
		}
		ctx->bytes_written.add((int64_t)data.size());
		ctx->files_done.increment();

		ret = unzGoToNextFile(pkg);
	}
	unzClose(pkg);
	ctx->finished.set();
	_ai_tpl_release_ctx(ctx);
}

static Dictionary _ai_tpl_error_result(const String &p_message, const Dictionary &p_details = Dictionary()) {
	Dictionary err_result;
	Dictionary ed;
	ed["code"] = "operation_failed";
	ed["message"] = p_message;
	ed["details"] = p_details;
	err_result["status"] = "error";
	err_result["error"] = ed;
	return err_result;
}

/* -------------------------------------------------------------------- */

bool AITemplateInstaller::start(const Callable &p_progress, const Callable &p_done) {
	if (phase != PHASE_INACTIVE) {
		return false;
	}
	progress_cb = p_progress;
	done_cb = p_done;
	start_ms = Time::get_singleton()->get_ticks_msec();
	last_progress_ms = start_ms;
	last_bytes = 0;
	tmp_path = EditorPaths::get_singleton()->get_temp_dir().path_join("ai_export_templates.tpz");

	HTTPRequest *req = memnew(HTTPRequest);
	req->set_use_threads(true);
	req->set_download_file(tmp_path);
	EditorNode::get_singleton()->add_child(req);
	req->connect("request_completed", callable_mp(this, &AITemplateInstaller::_on_download_completed));
	http_id = req->get_instance_id();

	phase = PHASE_DOWNLOADING;
	Error req_err = req->request(AI_EXPORT_TEMPLATES_URL);
	if (req_err != OK) {
		req->queue_free();
		http_id = ObjectID();
		// Report asynchronously so callers never observe done inside start();
		// phase stays DOWNLOADING until _finish resets it.
		callable_mp(this, &AITemplateInstaller::_finish)
				.bind(_ai_tpl_error_result(vformat("Could not start the template download (HTTPRequest error %d).", (int)req_err)))
				.call_deferred();
		return true;
	}

	print_line(vformat("AITemplateInstaller: downloading export templates from %s", AI_EXPORT_TEMPLATES_URL));
	if (progress_cb.is_valid()) {
		progress_cb.call("Downloading export templates...");
	}
	_schedule_tick(0.5f);
	return true;
}

void AITemplateInstaller::cancel() {
	if (phase == PHASE_INACTIVE) {
		return;
	}
	HTTPRequest *req = Object::cast_to<HTTPRequest>(ObjectDB::get_instance(http_id));
	if (req) {
		req->cancel_request();
		req->queue_free();
	}
	http_id = ObjectID();
	_release_extract_ctx(/*p_cancel=*/true);
	_cleanup_tmp();
	phase = PHASE_INACTIVE;
	done_cb = Callable();
	progress_cb = Callable();
}

AITemplateInstaller::~AITemplateInstaller() {
	cancel();
}

void AITemplateInstaller::_schedule_tick(float p_delay) {
	tick_gen++;
	SceneTree *tree = Object::cast_to<SceneTree>(OS::get_singleton()->get_main_loop());
	ERR_FAIL_NULL(tree);
	Ref<SceneTreeTimer> timer = tree->create_timer(p_delay);
	timer->connect("timeout", callable_mp(this, &AITemplateInstaller::_tick_gen_cb).bind(tick_gen), CONNECT_ONE_SHOT);
}

void AITemplateInstaller::_tick_gen_cb(uint32_t p_gen) {
	if (p_gen != tick_gen) {
		return; // Stale timer from a prior schedule.
	}
	_tick();
}

void AITemplateInstaller::_tick() {
	if (phase == PHASE_INACTIVE) {
		return;
	}
	uint64_t now_ms = Time::get_singleton()->get_ticks_msec();

	if (phase == PHASE_DOWNLOADING) {
		HTTPRequest *req = Object::cast_to<HTTPRequest>(ObjectDB::get_instance(http_id));
		if (!req) {
			phase = PHASE_INACTIVE;
			_finish(_ai_tpl_error_result("Template download aborted (request node disappeared)."));
			return;
		}
		int64_t got = req->get_downloaded_bytes();
		int64_t total = req->get_body_size();
		if (got > last_bytes) {
			last_bytes = got;
			last_progress_ms = now_ms;
		} else if (now_ms - last_progress_ms > 60000) {
			// Stall watchdog: no bytes for 60s.
			req->cancel_request();
			req->queue_free();
			http_id = ObjectID();
			_cleanup_tmp();
			phase = PHASE_INACTIVE;
			_finish(_ai_tpl_error_result("Template download stalled (no progress for 60 seconds). Check the network connection and retry."));
			return;
		}
		if (progress_cb.is_valid()) {
			if (total > 0) {
				progress_cb.call(vformat("Downloading export templates: %d / %d MB", got / 1000000, total / 1000000));
			} else {
				progress_cb.call(vformat("Downloading export templates: %d MB", got / 1000000));
			}
		}
		_schedule_tick(1.0f);
		return;
	}

	// PHASE_EXTRACTING
	AITplExtractContext *ctx = extract_ctx;
	if (!ctx) {
		return;
	}
	if (!ctx->finished.is_set()) {
		if (progress_cb.is_valid()) {
			progress_cb.call(vformat("Extracting export templates: %d / %d files", (int)ctx->files_done.get(), (int)ctx->files_total.get()));
		}
		_schedule_tick(0.5f);
		return;
	}

	String extract_error = ctx->error;
	int64_t bytes = ctx->bytes_written.get();
	_release_extract_ctx(/*p_cancel=*/false);
	_cleanup_tmp();
	phase = PHASE_INACTIVE;

	if (!extract_error.is_empty()) {
		_finish(_ai_tpl_error_result(extract_error));
		return;
	}

	Dictionary result_data;
	result_data["installed_path"] = AIExportActions::get_templates_dir();
	result_data["release_tag"] = AI_EXPORT_TEMPLATES_RELEASE_TAG;
	result_data["file_count"] = AIExportActions::count_template_files();
	result_data["total_bytes"] = bytes;
	result_data["elapsed_ms"] = (int64_t)(Time::get_singleton()->get_ticks_msec() - start_ms);
	result_data["note"] = "Templates are per-engine-version - this was a one-time install. Exports (and web serving) are now unblocked.";
	Dictionary ok_result;
	ok_result["status"] = "success";
	ok_result["result"] = result_data;
	print_line(vformat("AITemplateInstaller: export templates installed (%d files).", (int)result_data["file_count"]));
	_finish(ok_result);
}

void AITemplateInstaller::_on_download_completed(int p_result, int p_response_code, const PackedStringArray &p_headers, const PackedByteArray &p_body) {
	if (phase != PHASE_DOWNLOADING) {
		return; // Cancelled or stale.
	}
	HTTPRequest *req = Object::cast_to<HTTPRequest>(ObjectDB::get_instance(http_id));
	if (req) {
		req->queue_free();
	}
	http_id = ObjectID();

	if (p_result != (int)HTTPRequest::RESULT_SUCCESS || p_response_code != 200) {
		_cleanup_tmp();
		phase = PHASE_INACTIVE;
		Dictionary details;
		details["hint"] = "Likely a network problem or GitHub being unreachable. Retry later.";
		_finish(_ai_tpl_error_result(
				vformat("Template download failed (result %d, HTTP %d).", p_result, p_response_code), details));
		return;
	}

	if (progress_cb.is_valid()) {
		progress_cb.call("Download complete, extracting export templates...");
	}

	String dest_dir = AIExportActions::get_templates_dir();
	Error mk = DirAccess::make_dir_recursive_absolute(dest_dir);
	if (mk != OK && mk != ERR_ALREADY_EXISTS) {
		_cleanup_tmp();
		phase = PHASE_INACTIVE;
		_finish(_ai_tpl_error_result(vformat("Could not create the templates directory '%s'.", dest_dir)));
		return;
	}

	AITplExtractContext *ctx = memnew(AITplExtractContext);
	ctx->tpz_path = tmp_path;
	ctx->dest_dir = dest_dir;
	ctx->refs.set(2); // Worker task + installer.
	extract_ctx = ctx;
	phase = PHASE_EXTRACTING;
	WorkerThreadPool::get_singleton()->add_native_task(&_ai_tpl_extract_task, ctx, false, "AI: extract export templates");
	_schedule_tick(0.5f);
}

void AITemplateInstaller::_finish(const Dictionary &p_exec_result) {
	phase = PHASE_INACTIVE;
	Callable cb = done_cb;
	done_cb = Callable();
	progress_cb = Callable();
	if (cb.is_valid()) {
		cb.call(p_exec_result);
	}
}

void AITemplateInstaller::_release_extract_ctx(bool p_cancel) {
	if (!extract_ctx) {
		return;
	}
	if (p_cancel) {
		extract_ctx->cancel.set();
	}
	_ai_tpl_release_ctx(extract_ctx);
	extract_ctx = nullptr;
}

void AITemplateInstaller::_cleanup_tmp() {
	if (!tmp_path.is_empty() && FileAccess::exists(tmp_path)) {
		DirAccess::remove_absolute(tmp_path);
	}
}
