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
#include "scene/3d/camera_3d.h"
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

Dictionary exec_copy_file(const Dictionary &args) {
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

	if (source_path.is_empty() || dest_path.is_empty()) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"'source_path' and 'dest_path' must be non-empty");
	}
	if (source_path == dest_path) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"'source_path' and 'dest_path' must differ");
	}

	String source_abs = ps->globalize_path(source_path);
	String dest_abs = ps->globalize_path(dest_path);

	if (!FileAccess::exists(source_abs)) {
		return ai_create_error_result(AIErrorCodes::FILE_NOT_FOUND,
			vformat("Source file does not exist at '%s'", source_path));
	}

	bool dest_exists = FileAccess::exists(dest_abs);
	if (dest_exists && !overwrite) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("Destination file already exists at '%s'. Pass overwrite=true to replace it.", dest_path));
	}

	// Ensure destination directory exists. EditorFileSystem::copy_file expects
	// its parent dir to be in the filesystem cache, so we create+rescan if missing.
	String dest_dir_abs = dest_abs.get_base_dir();
	if (!DirAccess::exists(dest_dir_abs)) {
		Error dir_err = DirAccess::make_dir_recursive_absolute(dest_dir_abs);
		if (dir_err != OK) {
			return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
				vformat("Failed to create destination directory '%s'. Error: %d", dest_dir_abs, dir_err));
		}
	}

	// Snapshot pre-existing dest state so undo can restore it.
	PackedByteArray old_main_bytes;
	PackedByteArray old_sidecar_bytes;
	bool had_old_sidecar = false;
	if (dest_exists) {
		old_main_bytes = FileAccess::get_file_as_bytes(dest_abs);
		String sidecar_abs = dest_abs + ".import";
		if (FileAccess::exists(sidecar_abs)) {
			old_sidecar_bytes = FileAccess::get_file_as_bytes(sidecar_abs);
			had_old_sidecar = true;
		}
	}

	ai_log_verbose(vformat("Copying file '%s' -> '%s' (overwrite=%s)", source_path, dest_path, overwrite ? "true" : "false"));

	undo_redo->create_action("AI Copy File");
	undo_redo->add_do_method(ai_singleton, "_copy_file_via_efs", source_abs, dest_abs);
	// Undo: wipe whatever the copy produced, then restore any pre-existing dest.
	undo_redo->add_undo_method(ai_singleton, "_delete_file_with_sidecar", dest_abs);
	if (dest_exists) {
		undo_redo->add_undo_method(ai_singleton, "_write_binary_file", dest_abs, old_main_bytes);
		if (had_old_sidecar) {
			undo_redo->add_undo_method(ai_singleton, "_write_binary_file", dest_abs + ".import", old_sidecar_bytes);
		}
	}
	undo_redo->commit_action();

	int copied_size = 0;
	if (FileAccess::exists(dest_abs)) {
		copied_size = FileAccess::get_file_as_bytes(dest_abs).size();
	}

	Dictionary result_data;
	result_data["source_path"] = source_path;
	result_data["dest_path"] = dest_path;
	result_data["overwrite"] = overwrite;
	result_data["overwrote_existing"] = dest_exists;
	result_data["size"] = copied_size;

	print_line(vformat("AI: Executed copy_file. Source: %s, Dest: %s", source_path, dest_path));
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

	if (mode == "headless_smoke") {
		// For headless smoke test, try to execute a specific command if available
		// v0: log that it's not fully implemented yet
		ai_log_verbose("Execute 'run_project': headless_smoke mode requested but not fully implemented in v0.");
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			"headless_smoke mode is not implemented in this build");
	} else if (mode == "play") {
		Dictionary result_data;
		result_data["mode"] = mode;

		if (!scene_path.is_empty()) {
			if (!scene_path.begins_with("res://")) {
				return ai_create_error_result(AIErrorCodes::INVALID_PATH,
					vformat("scene_path must be a res:// path, got '%s'.", scene_path));
			}
			if (!FileAccess::exists(scene_path)) {
				return ai_create_error_result(AIErrorCodes::FILE_NOT_FOUND,
					vformat("Scene file '%s' does not exist.", scene_path));
			}
			EditorRunBar *run_bar = EditorRunBar::get_singleton();
			if (!run_bar) {
				return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
					"EditorRunBar singleton not found");
			}
			run_bar->play_custom_scene(scene_path);
			result_data["scene_path"] = scene_path;
			print_line(vformat("AI: Executed run_project (play mode, custom scene '%s').", scene_path));
			return ai_create_success_result(result_data);
		}

		// No scene_path: run the project's main scene via the standard command.
		EditorCommandPalette *command_palette = ei->get_command_palette();
		if (!command_palette) {
			return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
				"EditorCommandPalette not found");
		}
		command_palette->execute_command("editor/run_project");

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
	// async wait + screenshot via SceneTree timers; it launches the game by
	// routing back through here (execute_single_action). Forward the args so
	// scene_path reaches exec_run_project — screenshot timing args (wait_seconds,
	// screenshot_times_seconds) are consumed by the orchestrator and ignored here.
	return exec_run_project(args);
}

#ifdef TOOLS_ENABLED
// Force the given SubViewport to render exactly one frame, so cold-start captures
// (fresh project, user has never clicked into this viewport) produce a real image
// instead of a black/empty frame.
//
// Implementation notes:
// - Flush the MessageQueue first. CanvasItem::queue_redraw() uses call_deferred
//   to schedule its _redraw_callback, which is what actually submits draw
//   commands to the RenderingServer. Any CanvasItems with redraws still pending
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
// - p_canvas_transform (optional) is a framing transform applied to the viewport's
//   global canvas transform AFTER the MessageQueue flush and immediately before the
//   render. Order matters: when the 2D tab is visible, the flush runs
//   CanvasItemEditor::_draw_viewport, whose first act is to reset scene_root's
//   global canvas transform to the editor's own zoom/pan (which internally includes
//   EDSCALE). A framing transform set before the flush gets silently overwritten —
//   captures came out ~EDSCALE× too large, panned to the editor's current view.
//   Nothing runs between the post-flush write and rs->draw(), so this is race-free.
static void _force_render_subviewport(SubViewport *p_viewport, const Transform2D *p_canvas_transform = nullptr) {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!rs || !p_viewport->get_viewport_rid().is_valid()) {
		return;
	}
	// Flush pending deferred calls so CanvasItem _redraw_callbacks fire and submit
	// their draw commands to RS before we trigger the render pass.
	if (MessageQueue::get_singleton()) {
		MessageQueue::get_singleton()->flush();
	}
	if (p_canvas_transform) {
		p_viewport->set_global_canvas_transform(*p_canvas_transform);
	}
	SubViewport::UpdateMode prev_mode = p_viewport->get_update_mode();
	p_viewport->set_update_mode(SubViewport::UPDATE_ONCE);
	rs->draw(false);
	rs->sync(); // Block until render thread drains the queue (threaded RS mode).
	p_viewport->set_update_mode(prev_mode);
}

// Shared encoder: takes a SubViewport, returns a success/error result dict with
// a base64 PNG attached via _images for the orchestrator to forward to the model.
// p_canvas_transform is forwarded to _force_render_subviewport (see comment there).
static Dictionary _capture_subviewport_to_result(SubViewport *p_viewport, const String &p_label, const Transform2D *p_canvas_transform = nullptr) {
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
	_force_render_subviewport(p_viewport, p_canvas_transform);

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
	//
	// The transform is only COMPUTED here — it is applied inside
	// _force_render_subviewport, after the MessageQueue flush. Setting it now would
	// lose it: with the 2D tab visible, the flush runs CanvasItemEditor::_draw_viewport,
	// which resets scene_root's global canvas transform to the editor's own zoom/pan.
	bool has_frame_rect = false;
	Transform2D saved_transform;
	Transform2D frame_transform;
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

		frame_transform.scale_basis(Size2(zoom, zoom));
		frame_transform.columns[2] = -view_offset * zoom;

		saved_transform = sv->get_global_canvas_transform();
		has_frame_rect = true;
	}

	// Deliberately NO CanvasItemEditor::update_viewport() before the capture. The
	// grid/rulers/selection overlay draws into the CanvasItemEditor's own overlay
	// Control (in the editor window's viewport tree, a sibling of the
	// SubViewportContainer hosting scene_root), so it can never appear in a capture
	// of scene_root anyway — and queuing that redraw makes _draw_viewport fire during
	// the capture's MessageQueue flush, clobbering the framing transform.
	Dictionary result = _capture_subviewport_to_result(sv, "2D", has_frame_rect ? &frame_transform : nullptr);

	// Report the transform the render actually used, so the model can convert image
	// pixels to world units. Without frame_rect the capture shows the editor's current
	// view, whose internal zoom includes the editor display scale (EDSCALE) — image
	// pixels are NOT 1:1 with world units, so measuring layout off the raw image
	// silently over-reads by that factor (~2x on a 200%-scale display).
	// world_rect = [x, y, width, height] of the world-space region the image spans.
	if (sv && (String)result.get("status", "") == "success") {
		Transform2D used = sv->get_global_canvas_transform();
		Size2 vps = sv->get_size();
		real_t px_per_unit = used.get_scale().x;
		if (px_per_unit > 0.0 && vps.x > 0 && vps.y > 0) {
			Rect2 world_rect = used.affine_inverse().xform(Rect2(Point2(), vps));
			Dictionary rd = result["result"];
			rd["px_per_world_unit"] = px_per_unit;
			Array wr;
			wr.push_back(world_rect.position.x);
			wr.push_back(world_rect.position.y);
			wr.push_back(world_rect.size.x);
			wr.push_back(world_rect.size.y);
			rd["world_rect"] = wr;
		}
	}

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

#ifdef TOOLS_ENABLED
// Parse a [x, y, z] array Variant into a Vector3. Returns true on success.
// On failure, writes an INVALID_ARGS error result into r_error and returns false.
static bool _parse_vec3_arg(const Variant &v, const char *name, Vector3 &r_out, Dictionary &r_error) {
	if (v.get_type() != Variant::ARRAY) {
		r_error = ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			vformat("%s must be an array of 3 numbers [x, y, z].", name));
		return false;
	}
	Array arr = v;
	if (arr.size() != 3) {
		r_error = ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			vformat("%s must have exactly 3 elements [x, y, z], got %d.", name, arr.size()));
		return false;
	}
	for (int i = 0; i < 3; i++) {
		Variant::Type t = arr[i].get_type();
		if (t != Variant::INT && t != Variant::FLOAT) {
			r_error = ai_create_error_result(AIErrorCodes::INVALID_ARGS,
				vformat("%s[%d] must be a number.", name, i));
			return false;
		}
	}
	r_out = Vector3((real_t)(double)arr[0], (real_t)(double)arr[1], (real_t)(double)arr[2]);
	return true;
}
#endif

Dictionary exec_capture_3d_viewport(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	// Phase 3b: custom shot framing. When the caller provides shot_position +
	// shot_target we stand up a temporary SubViewport that shares the edited
	// scene's World3D, render one frame from a temp Camera3D posed at the requested
	// pose, and tear down. No perturbation of the editor's 4 viewports, and no
	// relationship to any Camera3D node in the edited scene — the shot parameters
	// are inputs to this capture only.
	const bool has_pos = args.has("shot_position");
	const bool has_tgt = args.has("shot_target");
	if (has_pos != has_tgt) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"shot_position and shot_target must both be provided together.");
	}

	if (has_pos) {
		// --- Custom-shot path ---
		Vector3 cam_pos, cam_tgt;
		Dictionary err;
		if (!_parse_vec3_arg(args["shot_position"], "shot_position", cam_pos, err)) {
			return err;
		}
		if (!_parse_vec3_arg(args["shot_target"], "shot_target", cam_tgt, err)) {
			return err;
		}
		if (cam_pos.is_equal_approx(cam_tgt)) {
			return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
				"shot_position and shot_target must not be equal.");
		}

		real_t fov = 70.0;
		if (args.has("shot_fov")) {
			Variant::Type t = args["shot_fov"].get_type();
			if (t != Variant::INT && t != Variant::FLOAT) {
				return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
					"shot_fov must be a number (degrees).");
			}
			fov = (real_t)(double)args["shot_fov"];
			if (fov <= 0.0 || fov >= 180.0) {
				return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
					vformat("shot_fov must be between 0 and 180 exclusive (got %f).", fov));
			}
		}

		int out_w = 1280;
		int out_h = 720;
		if (args.has("size")) {
			if (args["size"].get_type() != Variant::ARRAY) {
				return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
					"size must be an array [width, height].");
			}
			Array sz = args["size"];
			if (sz.size() != 2) {
				return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
					vformat("size must have exactly 2 elements [width, height], got %d.", sz.size()));
			}
			for (int i = 0; i < 2; i++) {
				Variant::Type t = sz[i].get_type();
				if (t != Variant::INT && t != Variant::FLOAT) {
					return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
						vformat("size[%d] must be a number.", i));
				}
			}
			out_w = (int)(double)sz[0];
			out_h = (int)(double)sz[1];
			if (out_w < 64 || out_h < 64) {
				return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
					vformat("size must be at least 64x64 (got %dx%d).", out_w, out_h));
			}
		}

		EditorNode *en = EditorNode::get_singleton();
		if (!en) {
			return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
				"EditorNode singleton is not available.");
		}
		SceneTree *st = en->get_tree();
		Ref<World3D> shared_world = (st && st->get_root()) ? st->get_root()->get_world_3d() : Ref<World3D>();
		if (shared_world.is_null()) {
			return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
				"Root World3D is not available.");
		}

		// Temp SubViewport parented under EditorNode so it gets proper ENTER_TREE
		// lifecycle (which drives RS viewport activation). Shares the root's
		// World3D — renders the exact same lights/environment/meshes as the editor
		// viewports. No scene cloning.
		SubViewport *tv = memnew(SubViewport);
		tv->set_size(Size2i(out_w, out_h));
		tv->set_disable_input(true);
		tv->set_update_mode(SubViewport::UPDATE_DISABLED); // _force_render flips to UPDATE_ONCE for us
		tv->set_world_3d(shared_world);
		en->add_child(tv);

		// Temp camera. cull_mask = (1<<20)-1 excludes editor layers (grid, gizmos,
		// selection boxes, misc tools) which live on layers 24-30 with RS-level
		// layer_mask filtering. See node_3d_editor_plugin.cpp around the camera
		// construction at line 5581 for the mask that *includes* those layers.
		Camera3D *cam = memnew(Camera3D);
		cam->set_cull_mask((1 << 20) - 1);
		Transform3D xf;
		xf.origin = cam_pos;
		xf = xf.looking_at(cam_tgt, Vector3(0, 1, 0));
		cam->set_transform(xf);
		cam->set_perspective(fov, 0.05, 4000.0); // near/far match Node3DEditorViewport defaults
		tv->add_child(cam);
		cam->make_current();

		// CRITICAL: Node3D dirties its transform on ENTER_TREE and queues a deferred
		// NOTIFICATION_TRANSFORM_CHANGED via SceneTree::xform_change_list. Until that
		// list is flushed, the RS-side camera stays at identity — so the capture would
		// render from the origin looking down -Z instead of our pose, producing only
		// the world environment (sky/ground, no geometry). MessageQueue::flush (inside
		// _force_render_subviewport) does NOT flush this list; force it synchronously.
		if (st) {
			st->flush_transform_notifications();
		}

		Dictionary result = _capture_subviewport_to_result(tv, "3D");

		en->remove_child(tv);
		memdelete(tv); // Camera is a child and is freed with the viewport.
		return result;
	}

	// --- Existing path: capture the last-used editor 3D viewport ---
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

