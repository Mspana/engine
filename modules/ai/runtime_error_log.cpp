// modules/ai/runtime_error_log.cpp

#include "runtime_error_log.h"

#include "ai_journal_writer.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/time.h"
#include "core/string/print_string.h"

#ifdef TOOLS_ENABLED
#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/script_editor_debugger.h"
#include "editor/editor_log.h"
#include "editor/editor_node.h"
#include "editor/gui/editor_run_bar.h"
#endif

AIRuntimeErrorLog::AIRuntimeErrorLog(AIJournalWriter *p_writer, const String &p_log_path_abs) {
	_writer = p_writer;
	_log_path_abs = p_log_path_abs;
}

void AIRuntimeErrorLog::ensure_wired() {
#ifdef TOOLS_ENABLED
	if (_wired) {
		return;
	}
	EditorRunBar *run_bar = EditorRunBar::get_singleton();
	EditorDebuggerNode *edn = EditorDebuggerNode::get_singleton();
	if (!run_bar || !edn) {
		return; // Editor not up yet; a later call retries.
	}
	ScriptEditorDebugger *dbg = edn->get_default_debugger();
	if (!dbg) {
		return;
	}
	run_bar->connect("play_pressed", callable_mp(this, &AIRuntimeErrorLog::_on_play_pressed));
	run_bar->connect("stop_pressed", callable_mp(this, &AIRuntimeErrorLog::_on_stop_pressed));
	dbg->connect("runtime_error_reported", callable_mp(this, &AIRuntimeErrorLog::_on_runtime_error));
	_wired = true;
#endif
}

void AIRuntimeErrorLog::note_run_requested(const String &p_scene, const String &p_mode) {
	_pending_scene = p_scene;
	_pending_mode = p_mode;
	_pending_ticks_ms = Time::get_singleton()->get_ticks_msec();
}

void AIRuntimeErrorLog::_enqueue(const Dictionary &p_entry) {
	if (_writer) {
		_writer->enqueue(_log_path_abs, p_entry);
	}
}

void AIRuntimeErrorLog::_maybe_rotate() {
	{
		Ref<FileAccess> f = FileAccess::open(_log_path_abs, FileAccess::READ);
		if (f.is_null() || f->get_length() < ROTATE_BYTES) {
			return;
		}
	}
	// Two-file rotation: current -> .prev, replacing any older .prev. A failed
	// rename (e.g. the writer thread mid-batch) just postpones rotation to the
	// next run start.
	String prev_path = _log_path_abs.get_basename() + ".prev." + _log_path_abs.get_extension();
	Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	if (da.is_null()) {
		return;
	}
	if (da->file_exists(prev_path)) {
		da->remove(prev_path);
	}
	da->rename(_log_path_abs, prev_path);
}

void AIRuntimeErrorLog::_on_play_pressed() {
#ifdef TOOLS_ENABLED
	if (_run_active) {
		// EditorRunBar stops before re-running, so this should not happen —
		// but a run must never dangle without an end marker.
		_write_run_end("stopped", false, 0);
	}

	_run_active = true;
	_run_id = "run_" + itos((int64_t)(Time::get_singleton()->get_unix_time_from_system() * 1000.0));
	_run_start_ticks_ms = Time::get_singleton()->get_ticks_msec();
	_error_count = 0;
	_warning_count = 0;
	_unique_error_keys.clear();
	_first_errors.clear();
	_exit_status = String();
	_has_exit_code = false;
	_exit_code = 0;
	_duration_ms = 0;

	// A note_run_requested older than 10s belongs to a launch that never
	// happened; treat this run as user-initiated then.
	bool agent_initiated = _pending_ticks_ms > 0 &&
			Time::get_singleton()->get_ticks_msec() - _pending_ticks_ms < 10000;
	String mode = agent_initiated ? _pending_mode : "editor_play";
	String pending_scene = _pending_scene;
	_pending_ticks_ms = 0;
	_pending_scene = String();
	_pending_mode = String();

	EditorRunBar *run_bar = EditorRunBar::get_singleton();
	String scene = run_bar ? run_bar->get_playing_scene() : String();
	if (scene.is_empty()) {
		scene = pending_scene;
	}

	_maybe_rotate();

	Dictionary e;
	e["type"] = "run_start";
	e["run_id"] = _run_id;
	e["ts"] = Time::get_singleton()->get_datetime_string_from_system(true);
	e["scene"] = scene;
	e["mode"] = mode;
	e["initiator"] = agent_initiated ? "agent" : "user";
	if (run_bar) {
		e["pid"] = (int64_t)run_bar->get_current_process();
	}
	_enqueue(e);
#endif
}

void AIRuntimeErrorLog::_on_runtime_error(const Dictionary &p_error) {
	if (!_run_active) {
		return; // Only errors raised during a game run belong in this log.
	}

	String severity = p_error.get("severity", "error");
	String message = p_error.get("message", "");
	String script = p_error.get("script", "");
	int line = p_error.get("line", 0);

	if (severity == "warning") {
		_warning_count++;
	} else {
		_error_count++;
		// Same dedup key as ScriptEditorDebugger::get_structured_errors():
		// same message at the same location is the same bug.
		String key = message + "|" + script + "|" + itos(line);
		if (!_unique_error_keys.has(key)) {
			_unique_error_keys.insert(key);
			if (_first_errors.size() < FIRST_ERRORS_MAX) {
				String verbatim = message;
				if (!script.is_empty()) {
					verbatim += vformat(" (%s:%d)", script, line);
				}
				_first_errors.push_back(verbatim);
			}
		}
	}

	// No duplicate collapsing here by design: an error-per-frame produces many
	// lines, and the structured file stays filterable like any log.
	Dictionary e;
	e["type"] = severity == "warning" ? "warning" : "error";
	e["run_id"] = _run_id;
	e["ts"] = Time::get_singleton()->get_datetime_string_from_system(true);
	e["t_ms"] = (int64_t)(Time::get_singleton()->get_ticks_msec() - _run_start_ticks_ms);
	e["message"] = message;
	if (!script.is_empty()) {
		e["script"] = script;
		e["line"] = line;
	}
	if (p_error.has("function")) {
		e["function"] = p_error["function"];
	}
	_enqueue(e);
}

void AIRuntimeErrorLog::_on_stop_pressed() {
#ifdef TOOLS_ENABLED
	if (!_run_active) {
		return;
	}
	// stop_pressed fires for every run end: editor/agent stop AND self-exit
	// (the debugger disconnect funnels into stop_playing()). The run bar
	// sampled the child before killing it, which is what tells the cases apart.
	EditorRunBar *run_bar = EditorRunBar::get_singleton();
	bool exited = run_bar && run_bar->get_last_run_child_exited();
	int exit_code = exited ? run_bar->get_last_run_child_exit_code() : 0;
	String status = exited ? (exit_code == 0 ? "exited" : "crashed") : "stopped";
	_write_run_end(status, exited, exit_code);
#endif
}

void AIRuntimeErrorLog::_write_run_end(const String &p_exit_status, bool p_has_exit_code, int p_exit_code) {
	_exit_status = p_exit_status;
	_has_exit_code = p_has_exit_code;
	_exit_code = p_exit_code;
	_duration_ms = Time::get_singleton()->get_ticks_msec() - _run_start_ticks_ms;
	_run_active = false;

	Dictionary e;
	e["type"] = "run_end";
	e["run_id"] = _run_id;
	e["ts"] = Time::get_singleton()->get_datetime_string_from_system(true);
	e["duration_ms"] = (int64_t)_duration_ms;
	e["exit_status"] = _exit_status;
	if (_has_exit_code) {
		e["exit_code"] = _exit_code;
	}
	e["error_count"] = _error_count;
	e["unique_error_count"] = (int)_unique_error_keys.size();
	e["warning_count"] = _warning_count;
#ifdef TOOLS_ENABLED
	if (_exit_status == "crashed") {
		// Crashes carry the exact output tail — the crash text (script stack,
		// engine abort message) usually only exists here, not as a debugger
		// error, because the connection died with the process.
		EditorLog *log = EditorNode::get_log();
		if (log) {
			e["output_tail"] = log->get_recent_messages_text(200);
		}
	}
#endif
	_enqueue(e);
}

Dictionary AIRuntimeErrorLog::get_run_digest() const {
	Dictionary d;
	if (_run_id.is_empty()) {
		return d; // No run observed since editor start.
	}
	d["run_id"] = _run_id;
	d["running"] = _run_active;
	d["exit_status"] = _run_active ? "running" : _exit_status;
	d["crashed"] = !_run_active && _exit_status == "crashed";
	if (!_run_active && _has_exit_code) {
		d["exit_code"] = _exit_code;
	}
	d["error_count"] = _error_count;
	d["unique_error_count"] = (int)_unique_error_keys.size();
	d["warning_count"] = _warning_count;
	if (!_first_errors.is_empty()) {
		d["first_errors"] = _first_errors.duplicate();
	}
	d["duration_ms"] = (int64_t)(_run_active
			? Time::get_singleton()->get_ticks_msec() - _run_start_ticks_ms
			: _duration_ms);
	d["log_path"] = get_log_path_user();
	return d;
}
