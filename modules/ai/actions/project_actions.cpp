// modules/ai/actions/project_actions.cpp
// Project settings-related action implementations for the AI module

#include "project_actions.h"
#include "action_common.h"

#include "../ai.h" // For AI::get_singleton() and file helper methods

#include "core/config/project_settings.h"
#include "core/variant/variant.h"
#include "core/variant/array.h"
#include "core/object/object.h"
#include "core/io/resource_loader.h"
#include "core/io/file_access.h"
#include "core/io/dir_access.h"
#include "editor/editor_interface.h"
#include "editor/editor_command_palette.h"
#include "editor/editor_node.h"
#include "editor/gui/editor_run_bar.h"
#include "editor/plugins/canvas_item_editor_plugin.h"
#include "editor/plugins/node_3d_editor_plugin.h"

#include "core/core_bind.h"
#include "core/io/image.h"
#include "core/object/message_queue.h"
#include "scene/main/viewport.h"
#include "servers/rendering_server.h"

namespace AIProjectActions {

Dictionary exec_set_project_setting(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	EditorUndoRedoManager *undo_redo = ai_get_undo_redo();
	if (!undo_redo) {
		return ai_create_error_result(AIErrorCodes::NO_UNDO_REDO,
			"EditorUndoRedoManager singleton not found");
	}

	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			"ProjectSettings singleton not available");
	}

	if (!args.has("key") || args["key"].get_type() != Variant::STRING) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"'key' must be a string");
	}
	if (!args.has("value")) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"'value' is required");
	}

	String key = args["key"];
	Variant value = args["value"];

	// Get old value for undo
	bool setting_existed = ps->has_setting(key);
	Variant old_value;
	if (setting_existed) {
		old_value = ps->get_setting(key);
	}

	ai_log_verbose(vformat("Setting project setting '%s' to '%s'", key, value));

	// Wrap in UndoRedo
	undo_redo->create_action("AI Set Project Setting");
	undo_redo->add_do_method(ps, "set_setting", key, value);
	undo_redo->add_do_method(ps, "save");
	if (setting_existed) {
		// Setting existed, restore old value on undo
		undo_redo->add_undo_method(ps, "set_setting", key, old_value);
	} else {
		// Setting didn't exist, remove it on undo
		undo_redo->add_undo_method(ps, "clear", key);
	}
	undo_redo->add_undo_method(ps, "save");
	undo_redo->commit_action();

	Dictionary result_data;
	result_data["key"] = key;
	result_data["value"] = value;
	result_data["had_previous_value"] = setting_existed;
	if (setting_existed) {
		result_data["previous_value"] = old_value;
	}

	print_line(vformat("AI: Executed set_project_setting. Key: %s", key));
	return ai_create_success_result(result_data);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

Dictionary exec_get_project_settings(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			"ProjectSettings singleton not available");
	}

	String prefix = args.get("prefix", String());
	Array keys_array;
	if (args.has("keys") && args["keys"].get_type() == Variant::ARRAY) {
		keys_array = args["keys"];
	}
	bool include_defaults = args.get("include_defaults", false);

	Dictionary settings_dict;

	if (!keys_array.is_empty()) {
		// Query specific keys
		for (int i = 0; i < keys_array.size(); i++) {
			if (keys_array[i].get_type() != Variant::STRING) {
				continue;
			}
			String key = keys_array[i];

			if (!ps->has_setting(key)) {
				if (include_defaults) {
					// Try to get default value (may return Variant() if not found)
					Variant value = ps->get_setting(key);
					if (value.get_type() != Variant::NIL) {
						settings_dict[key] = value;
						print_line(vformat("%s = %s", key, value));
					}
				}
				continue;
			}

			Variant value = ps->get_setting(key);
			settings_dict[key] = value;
			print_line(vformat("%s = %s", key, value));
		}
	} else {
		// Iterate over all settings using get_property_list
		List<PropertyInfo> property_list;
		ps->get_property_list(&property_list);

		for (const PropertyInfo &prop : property_list) {
			String key = prop.name;

			// Filter by prefix if provided
			if (!prefix.is_empty() && !key.begins_with(prefix)) {
				continue;
			}

			// Skip if setting doesn't exist (unless include_defaults)
			if (!ps->has_setting(key)) {
				if (include_defaults) {
					Variant value = ps->get_setting(key);
					if (value.get_type() != Variant::NIL) {
						settings_dict[key] = value;
						print_line(vformat("%s = %s", key, value));
					}
				}
				continue;
			}

			Variant value = ps->get_setting(key);
			settings_dict[key] = value;
			print_line(vformat("%s = %s", key, value));
		}
	}

	Dictionary result_data;
	result_data["settings"] = settings_dict;
	result_data["count"] = settings_dict.size();

	if (settings_dict.is_empty()) {
		ai_log_verbose("Execute 'get_project_settings': No settings found matching criteria.");
	}

	return ai_create_success_result(result_data);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

Dictionary exec_create_autoload_singleton(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	EditorUndoRedoManager *undo_redo = ai_get_undo_redo();
	if (!undo_redo) {
		return ai_create_error_result(AIErrorCodes::NO_UNDO_REDO,
			"EditorUndoRedoManager singleton not found");
	}

	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			"ProjectSettings singleton not available");
	}

	if (!args.has("name") || args["name"].get_type() != Variant::STRING) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"'name' must be a string");
	}
	if (!args.has("script_path") || args["script_path"].get_type() != Variant::STRING) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"'script_path' must be a string");
	}

	String name = args["name"];
	String script_path = args["script_path"];
	bool enabled = args.get("enabled", true);

	// Validate script path exists
	if (!ResourceLoader::exists(script_path)) {
		return ai_create_error_result(AIErrorCodes::FILE_NOT_FOUND,
			vformat("Script file does not exist at path '%s'", script_path));
	}

	// Format the autoload value: "*<path>" for singleton (enabled), "<path>" for non-singleton (disabled)
	String autoload_value;
	if (enabled) {
		autoload_value = "*" + script_path;
	} else {
		autoload_value = script_path;
	}

	String autoload_key = "autoload/" + name;

	// Get old value for undo
	Variant old_value;
	bool had_autoload = ps->has_setting(autoload_key);
	if (had_autoload) {
		old_value = ps->get_setting(autoload_key);
	}

	ai_log_verbose(vformat("Creating autoload singleton '%s' with path '%s' (enabled: %s)", name, script_path, enabled ? "true" : "false"));

	// Wrap in UndoRedo
	undo_redo->create_action("AI Create Autoload Singleton");
	undo_redo->add_do_method(ps, "set_setting", autoload_key, autoload_value);
	undo_redo->add_do_method(ps, "save");
	if (had_autoload) {
		// Autoload existed, restore old value on undo
		undo_redo->add_undo_method(ps, "set_setting", autoload_key, old_value);
	} else {
		// Autoload didn't exist, remove it on undo
		undo_redo->add_undo_method(ps, "clear", autoload_key);
	}
	undo_redo->add_undo_method(ps, "save");
	undo_redo->commit_action();

	Dictionary result_data;
	result_data["name"] = name;
	result_data["script_path"] = script_path;
	result_data["enabled"] = enabled;
	result_data["had_previous_autoload"] = had_autoload;

	print_line(vformat("AI: Executed create_autoload_singleton. Name: %s, Path: %s, Enabled: %s", name, script_path, enabled ? "true" : "false"));
	return ai_create_success_result(result_data);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

Dictionary exec_remove_autoload_singleton(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	EditorUndoRedoManager *undo_redo = ai_get_undo_redo();
	if (!undo_redo) {
		return ai_create_error_result(AIErrorCodes::NO_UNDO_REDO,
			"EditorUndoRedoManager singleton not found");
	}

	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			"ProjectSettings singleton not available");
	}

	if (!args.has("name") || args["name"].get_type() != Variant::STRING) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"'name' must be a string");
	}

	String name = args["name"];
	String autoload_key = "autoload/" + name;

	// Check if autoload exists
	if (!ps->has_setting(autoload_key)) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("Autoload '%s' does not exist", name));
	}

	// Get old value for undo
	Variant old_value = ps->get_setting(autoload_key);

	ai_log_verbose(vformat("Removing autoload singleton '%s'", name));

	// Wrap in UndoRedo
	undo_redo->create_action("AI Remove Autoload Singleton");
	undo_redo->add_do_method(ps, "clear", autoload_key);
	undo_redo->add_do_method(ps, "save");
	undo_redo->add_undo_method(ps, "set_setting", autoload_key, old_value);
	undo_redo->add_undo_method(ps, "save");
	undo_redo->commit_action();

	Dictionary result_data;
	result_data["name"] = name;
	result_data["previous_value"] = old_value;

	print_line(vformat("AI: Executed remove_autoload_singleton. Name: %s", name));
	return ai_create_success_result(result_data);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

Dictionary exec_import_asset(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	EditorUndoRedoManager *undo_redo = ai_get_undo_redo();
	if (!undo_redo) {
		return ai_create_error_result(AIErrorCodes::NO_UNDO_REDO,
			"EditorUndoRedoManager singleton not found");
	}

	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			"ProjectSettings singleton not available");
	}

	AI *ai_singleton = AI::get_singleton();
	if (!ai_singleton) {
		return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			"AI singleton not found");
	}

	if (!args.has("source_path") || args["source_path"].get_type() != Variant::STRING) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"'source_path' must be a string");
	}
	if (!args.has("dest_path") || args["dest_path"].get_type() != Variant::STRING) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"'dest_path' must be a string");
	}

	String source_path = args["source_path"];
	String dest_path = args["dest_path"];
	bool overwrite = args.get("overwrite", false);

	// Resolve res:// or user:// source paths to absolute OS paths so the AI
	// doesn't need to know OS-level paths for files already in the project.
	if (source_path.begins_with("res://") || source_path.begins_with("user://")) {
		source_path = ps->globalize_path(source_path);
	}

	// Validate source file exists
	if (!FileAccess::exists(source_path)) {
		return ai_create_error_result(AIErrorCodes::FILE_NOT_FOUND,
			vformat("Source file does not exist at path '%s'", source_path));
	}

	// Convert dest_path to absolute OS path
	String dest_abs_path = ps->globalize_path(dest_path);

	// Check if dest exists
	bool dest_exists = FileAccess::exists(dest_abs_path);
	if (dest_exists && !overwrite) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("Destination file already exists at '%s' and overwrite is false", dest_path));
	}

	// Read source file
	PackedByteArray source_bytes = FileAccess::get_file_as_bytes(source_path);
	if (source_bytes.is_empty()) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("Source file at '%s' is empty or could not be read", source_path));
	}

	// Read old content for undo if dest exists
	PackedByteArray old_bytes;
	if (dest_exists) {
		old_bytes = FileAccess::get_file_as_bytes(dest_abs_path);
	}

	// Ensure destination directory exists
	String dest_dir = dest_abs_path.get_base_dir();
	if (!DirAccess::exists(dest_dir)) {
		Error dir_err = DirAccess::make_dir_recursive_absolute(dest_dir);
		if (dir_err != OK) {
			return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
				vformat("Failed to create destination directory '%s'. Error: %d", dest_dir, dir_err));
		}
	}

	ai_log_verbose(vformat("Importing asset from '%s' to '%s' (overwrite: %s)", source_path, dest_path, overwrite ? "true" : "false"));

	// Wrap in UndoRedo
	undo_redo->create_action("AI Import Asset");
	undo_redo->add_do_method(ai_singleton, "_write_binary_file", dest_abs_path, source_bytes);
	if (dest_exists) {
		// Restore old content
		undo_redo->add_undo_method(ai_singleton, "_write_binary_file", dest_abs_path, old_bytes);
	} else {
		// Delete file
		undo_redo->add_undo_method(ai_singleton, "_delete_script_file", dest_abs_path);
	}
	undo_redo->commit_action();

	Dictionary result_data;
	result_data["source_path"] = source_path;
	result_data["dest_path"] = dest_path;
	result_data["overwrite"] = overwrite;
	result_data["overwrote_existing"] = dest_exists;
	result_data["size"] = source_bytes.size();

	print_line(vformat("AI: Executed import_asset. Source: %s, Dest: %s", source_path, dest_path));
	return ai_create_success_result(result_data);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

Dictionary exec_delete_asset(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	EditorUndoRedoManager *undo_redo = ai_get_undo_redo();
	if (!undo_redo) {
		return ai_create_error_result(AIErrorCodes::NO_UNDO_REDO,
			"EditorUndoRedoManager singleton not found");
	}

	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			"ProjectSettings singleton not available");
	}

	AI *ai_singleton = AI::get_singleton();
	if (!ai_singleton) {
		return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			"AI singleton not found");
	}

	if (!args.has("asset_path") || args["asset_path"].get_type() != Variant::STRING) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"'asset_path' must be a string");
	}

	String asset_path = args["asset_path"];

	// Convert asset_path to absolute OS path
	String asset_abs_path = ps->globalize_path(asset_path);

	// Check if file exists
	if (!FileAccess::exists(asset_abs_path)) {
		return ai_create_error_result(AIErrorCodes::FILE_NOT_FOUND,
			vformat("Asset file does not exist at path '%s'", asset_path));
	}

	// Read old bytes for undo
	PackedByteArray old_bytes = FileAccess::get_file_as_bytes(asset_abs_path);

	ai_log_verbose(vformat("Deleting asset at '%s'", asset_path));

	// Wrap in UndoRedo
	undo_redo->create_action("AI Delete Asset");
	undo_redo->add_do_method(ai_singleton, "_delete_script_file", asset_abs_path);
	undo_redo->add_undo_method(ai_singleton, "_write_binary_file", asset_abs_path, old_bytes);
	undo_redo->commit_action();

	Dictionary result_data;
	result_data["asset_path"] = asset_path;
	result_data["size"] = old_bytes.size();

	print_line(vformat("AI: Executed delete_asset. Path: %s", asset_path));
	return ai_create_success_result(result_data);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

Dictionary exec_run_project(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			"EditorInterface singleton not found");
	}

	String mode = args.get("mode", "play");
	String scene_path = args.get("scene_path", String());

	// Try to get EditorRunBar if available
	// For now, use command palette approach similar to save_scene
	EditorCommandPalette *command_palette = ei->get_command_palette();
	if (!command_palette) {
		return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			"EditorCommandPalette not found");
	}

	if (mode == "headless_smoke") {
		// For headless smoke test, try to execute a specific command if available
		// v0: log that it's not fully implemented yet
		ai_log_verbose("Execute 'run_project': headless_smoke mode requested but not fully implemented in v0.");
		// Try to execute the command anyway - it might work if the command exists
		// For now, just log and return error
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			"headless_smoke mode is not implemented in this build");
	} else if (mode == "play") {
		// Execute the standard run project command
		if (!scene_path.is_empty()) {
			// If scene_path is provided, we'd need to use EditorRunBar::play_custom_scene
			// For v0, just log that custom scene path is not fully supported
			ai_log_verbose(vformat("Execute 'run_project': scene_path '%s' provided but custom scene execution not fully implemented in v0. Running main scene instead.", scene_path));
		}

		// Execute the standard "editor/run_project" command
		command_palette->execute_command("editor/run_project");

		Dictionary result_data;
		result_data["mode"] = mode;
		if (!scene_path.is_empty()) {
			result_data["requested_scene_path"] = scene_path;
			result_data["note"] = "Custom scene path not fully supported, ran main scene instead";
		}

		print_line("AI: Executed run_project (play mode).");
		return ai_create_success_result(result_data);
	} else {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			vformat("Unknown mode '%s'. Supported modes: 'play', 'headless_smoke'", mode));
	}
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

Dictionary exec_run_and_screenshot(const Dictionary &args) {
	// In agentic mode the orchestrator intercepts this action and handles
	// async wait + screenshot via SceneTree timers. This fallback is for
	// direct/legacy calls only — it just starts the game.
	return exec_run_project(Dictionary());
}

#ifdef TOOLS_ENABLED
// Force the given SubViewport to render exactly one frame, so cold-start captures
// (fresh project, user has never clicked into this viewport) produce a real image
// instead of a black/empty frame.
//
// Implementation notes:
// - Flush the MessageQueue first. CanvasItem::queue_redraw() uses call_deferred
//   to schedule its _redraw_callback, which is what actually submits draw
//   commands to the RenderingServer. On a tab where CanvasItems were recently
//   queued (e.g. after exec_capture_2d_viewport called update_viewport()), we
//   need those deferred callbacks to fire BEFORE we draw — otherwise the
//   viewport renders with an empty command list and we get the clear color.
// - Scene-level set_update_mode(UPDATE_ONCE) writes through to RS and keeps the
//   Viewport node's cached state in sync.
// - Unlike the preview plugins, we do NOT deactivate the editor's root viewport.
//   Their target is an off-screen viewport unrelated to the scene tree, so
//   deactivating the root is a harmless optimization. Our target IS part of the
//   editor's scene tree; deactivating the root would cancel the render we want.
// - In threaded RS mode, RS::draw(false) only enqueues the draw — so we follow
//   up with RS::sync(), which blocks until the render thread drains the queue.
// - We never use the preview-plugin frame_pre_draw + semaphore branch. That
//   pattern only works from a worker thread; on the main thread it deadlocks.
static void _force_render_subviewport(SubViewport *p_viewport) {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!rs || !p_viewport->get_viewport_rid().is_valid()) {
		return;
	}
	// Flush pending deferred calls so CanvasItem _redraw_callbacks fire and submit
	// their draw commands to RS before we trigger the render pass.
	if (MessageQueue::get_singleton()) {
		MessageQueue::get_singleton()->flush();
	}
	SubViewport::UpdateMode prev_mode = p_viewport->get_update_mode();
	p_viewport->set_update_mode(SubViewport::UPDATE_ONCE);
	rs->draw(false);
	rs->sync(); // Block until render thread drains the queue (threaded RS mode).
	p_viewport->set_update_mode(prev_mode);
}

// Shared encoder: takes a SubViewport, returns a success/error result dict with
// a base64 PNG attached via _images for the orchestrator to forward to the model.
static Dictionary _capture_subviewport_to_result(SubViewport *p_viewport, const String &p_label) {
	if (!p_viewport) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("%s viewport is not available (editor not ready?).", p_label));
	}

	Ref<ViewportTexture> tex = p_viewport->get_texture();
	if (tex.is_null()) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("%s viewport texture is null.", p_label));
	}

	// Force a fresh render before reading the texture. Harmless when the viewport
	// is already rendering (adds one frame of latency); essential when it is not
	// (cold-start: user has never interacted with this viewport, so it has never
	// produced a frame and get_image() would return black).
	_force_render_subviewport(p_viewport);

	Ref<Image> img = tex->get_image();
	if (img.is_null() || img->is_empty()) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("%s viewport image is empty — the panel may be hidden or not yet rendered.", p_label));
	}

	Vector<uint8_t> png_bytes = img->save_png_to_buffer();
	if (png_bytes.is_empty()) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("%s viewport PNG encoding failed.", p_label));
	}

	PackedByteArray pba;
	pba.resize(png_bytes.size());
	memcpy(pba.ptrw(), png_bytes.ptr(), png_bytes.size());
	String b64 = CoreBind::Marshalls::get_singleton()->raw_to_base64(pba);

	Dictionary result_data;
	result_data["width"] = img->get_width();
	result_data["height"] = img->get_height();
	// Matches run_and_screenshot's shape: the orchestrator strips screenshot_b64 from
	// the wire content, attaches it to the message as _images, the UI pill renders it,
	// and the chat store persists it to disk as <call_id>.png.
	result_data["screenshot_b64"] = b64;

	return ai_create_success_result(result_data);
}
#endif

Dictionary exec_capture_2d_viewport(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	// The 2D editor displays the shared edited-scene SubViewport (EditorNode::scene_root).
	// Capturing it gives us exactly what the user sees in the 2D canvas, including the
	// current pan/zoom applied via the canvas transform.
	CanvasItemEditor *cie = CanvasItemEditor::get_singleton();
	if (!cie) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			"CanvasItemEditor singleton is not available.");
	}
	EditorNode *en = EditorNode::get_singleton();
	if (!en) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			"EditorNode singleton is not available.");
	}
	SubViewport *sv = en->get_scene_root();
	// EditorNode disables 2D rendering on scene_root whenever the 2D editor tab is
	// not the active main screen (see editor_node.cpp NOTIFICATION_READY and
	// CanvasItemEditorPlugin::make_visible). Without re-enabling it, captures from
	// the Script tab (or any non-2D tab, including cold start) render only the
	// clear color — a grey image. Temporarily re-enable 2D + environment for the
	// duration of the capture, then restore if the 2D tab isn't currently visible.
	//
	// Additionally, scene_root's size is driven by the SubViewportContainer that
	// hosts the 2D canvas. When that container has never been laid out (cold start
	// while on a different tab), scene_root's size is 2×2 — capture would return a
	// 2×2 image. Force a reasonable size for the capture, then restore.
	RenderingServer *rs = RenderingServer::get_singleton();
	RID vp_rid = sv ? sv->get_viewport_rid() : RID();
	const bool tab_hidden = sv && rs && vp_rid.is_valid() && !cie->is_visible_in_tree();
	Size2i prev_size;
	if (tab_hidden) {
		rs->viewport_set_disable_2d(vp_rid, false);
		rs->viewport_set_environment_mode(vp_rid, RS::VIEWPORT_ENVIRONMENT_ENABLED);
		prev_size = sv->get_size();
		if (prev_size.x < 64 || prev_size.y < 64) {
			int w = GLOBAL_GET("display/window/size/viewport_width");
			int h = GLOBAL_GET("display/window/size/viewport_height");
			if (w < 64 || h < 64) { w = 1280; h = 720; }
			// scene_root's parent (SubViewportContainer) has stretch enabled, which
			// causes set_size() to be rejected with a warning. set_size_force bypasses
			// that — required because the container hasn't laid out scene_root yet
			// (cold start while on a non-2D tab leaves it at the default 2×2).
			sv->set_size_force(Size2i(w, h));
		}
	}

	// Optional custom framing. When `frame_rect` is provided, we override scene_root's
	// global canvas transform so the capture frames the requested world-space rect
	// (fit, preserves aspect ratio). Computed AFTER the size_force above so the
	// framing math sees the post-resize viewport size. Restored after the capture.
	// Mirrors the editor's own zoom/offset → Transform2D math in
	// CanvasItemEditor::_draw_viewport.
	bool has_frame_rect = false;
	Transform2D saved_transform;
	if (sv && args.has("frame_rect")) {
		Variant fr_v = args["frame_rect"];
		if (fr_v.get_type() != Variant::ARRAY) {
			return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
				"frame_rect must be an array of 4 numbers [x, y, width, height].");
		}
		Array fr_arr = fr_v;
		if (fr_arr.size() != 4) {
			return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
				vformat("frame_rect must have exactly 4 elements [x, y, width, height], got %d.", fr_arr.size()));
		}
		for (int i = 0; i < 4; i++) {
			Variant::Type t = fr_arr[i].get_type();
			if (t != Variant::INT && t != Variant::FLOAT) {
				return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
					vformat("frame_rect[%d] must be a number.", i));
			}
		}
		real_t rx = (real_t)(double)fr_arr[0];
		real_t ry = (real_t)(double)fr_arr[1];
		real_t rw = (real_t)(double)fr_arr[2];
		real_t rh = (real_t)(double)fr_arr[3];
		if (rw <= 0.0 || rh <= 0.0) {
			return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
				vformat("frame_rect width and height must be > 0 (got %f x %f).", rw, rh));
		}
		Size2 vp_size = sv->get_size();
		if (vp_size.x < 64 || vp_size.y < 64) {
			return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
				vformat("scene_root size is too small for framing (%d x %d).", (int)vp_size.x, (int)vp_size.y));
		}

		real_t zoom = MIN(vp_size.x / rw, vp_size.y / rh);
		Vector2 center(rx + rw * 0.5, ry + rh * 0.5);
		Vector2 view_offset = center - Vector2(vp_size.x, vp_size.y) / (2.0 * zoom);

		Transform2D t;
		t.scale_basis(Size2(zoom, zoom));
		t.columns[2] = -view_offset * zoom;

		saved_transform = sv->get_global_canvas_transform();
		sv->set_global_canvas_transform(t);
		has_frame_rect = true;
	}

	// Queue a redraw of the 2D canvas overlay (grid, rulers, selection, guides) so
	// it appears on top of the scene contents. The MessageQueue flush inside
	// _capture_subviewport_to_result fires the deferred _redraw_callback before draw.
	cie->update_viewport();
	Dictionary result = _capture_subviewport_to_result(sv, "2D");

	if (has_frame_rect) {
		// Restore the editor's transform synchronously (so anything reading
		// scene_root->get_global_canvas_transform() in the brief window before the
		// next editor redraw sees the right value), then queue a redraw — the
		// editor's _draw_viewport will re-apply its own zoom/offset on the next
		// frame anyway, but the explicit restore avoids a stale-state window.
		sv->set_global_canvas_transform(saved_transform);
		cie->update_viewport();
	}
	if (tab_hidden) {
		rs->viewport_set_disable_2d(vp_rid, true);
		rs->viewport_set_environment_mode(vp_rid, RS::VIEWPORT_ENVIRONMENT_DISABLED);
		if (sv->get_size() != prev_size) {
			sv->set_size_force(prev_size);
		}
	}
	return result;
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

Dictionary exec_capture_3d_viewport(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	Node3DEditor *n3d = Node3DEditor::get_singleton();
	if (!n3d) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			"Node3DEditor singleton is not available.");
	}
	Node3DEditorViewport *v = n3d->get_last_used_viewport();
	if (!v) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			"No 3D editor viewport is available.");
	}
	SubViewport *sv = v->get_viewport_node();
	return _capture_subviewport_to_result(sv, "3D");
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

} // namespace AIProjectActions

