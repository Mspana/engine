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
#include "editor/gui/editor_run_bar.h"

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

} // namespace AIProjectActions

