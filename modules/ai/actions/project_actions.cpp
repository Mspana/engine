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

namespace AIProjectActions {

bool exec_set_project_setting(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	EditorUndoRedoManager *undo_redo = ai_get_undo_redo();
	if (!undo_redo) {
		ai_log_error("Execute 'set_project_setting': EditorUndoRedoManager singleton not found.");
		return false;
	}

	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		ai_log_error("Execute 'set_project_setting': ProjectSettings singleton not available.");
		return false;
	}

	if (!args.has("key") || args["key"].get_type() != Variant::STRING) {
		ai_log_error("Execute 'set_project_setting': 'key' must be a string.");
		return false;
	}
	if (!args.has("value")) {
		ai_log_error("Execute 'set_project_setting': 'value' is required.");
		return false;
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

	print_line(vformat("AI: Executed set_project_setting. Key: %s", key));
	return true;
#else
	ai_log_error("Execute 'set_project_setting': Editor API not available in non-editor builds.");
	return false;
#endif
}

bool exec_get_project_settings(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		ai_log_error("Execute 'get_project_settings': ProjectSettings singleton not available.");
		return false;
	}

	String prefix = args.get("prefix", String());
	Array keys_array;
	if (args.has("keys") && args["keys"].get_type() == Variant::ARRAY) {
		keys_array = args["keys"];
	}
	bool include_defaults = args.get("include_defaults", false);

	int found_count = 0;

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
						print_line(vformat("%s = %s", key, value));
						found_count++;
					}
				}
				continue;
			}

			Variant value = ps->get_setting(key);
			print_line(vformat("%s = %s", key, value));
			found_count++;
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
						print_line(vformat("%s = %s", key, value));
						found_count++;
					}
				}
				continue;
			}

			Variant value = ps->get_setting(key);
			print_line(vformat("%s = %s", key, value));
			found_count++;
		}
	}

	if (found_count == 0) {
		ai_log_verbose("Execute 'get_project_settings': No settings found matching criteria.");
		return false;
	}

	return true;
#else
	ai_log_error("Execute 'get_project_settings': Editor API not available in non-editor builds.");
	return false;
#endif
}

bool exec_create_autoload_singleton(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	EditorUndoRedoManager *undo_redo = ai_get_undo_redo();
	if (!undo_redo) {
		ai_log_error("Execute 'create_autoload_singleton': EditorUndoRedoManager singleton not found.");
		return false;
	}

	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		ai_log_error("Execute 'create_autoload_singleton': ProjectSettings singleton not available.");
		return false;
	}

	if (!args.has("name") || args["name"].get_type() != Variant::STRING) {
		ai_log_error("Execute 'create_autoload_singleton': 'name' must be a string.");
		return false;
	}
	if (!args.has("script_path") || args["script_path"].get_type() != Variant::STRING) {
		ai_log_error("Execute 'create_autoload_singleton': 'script_path' must be a string.");
		return false;
	}

	String name = args["name"];
	String script_path = args["script_path"];
	bool enabled = args.get("enabled", true);

	// Validate script path exists
	if (!ResourceLoader::exists(script_path)) {
		ai_log_error(vformat("Execute 'create_autoload_singleton': Script file does not exist at path '%s'.", script_path));
		return false;
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

	print_line(vformat("AI: Executed create_autoload_singleton. Name: %s, Path: %s, Enabled: %s", name, script_path, enabled ? "true" : "false"));
	return true;
#else
	ai_log_error("Execute 'create_autoload_singleton': Editor API not available in non-editor builds.");
	return false;
#endif
}

bool exec_remove_autoload_singleton(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	EditorUndoRedoManager *undo_redo = ai_get_undo_redo();
	if (!undo_redo) {
		ai_log_error("Execute 'remove_autoload_singleton': EditorUndoRedoManager singleton not found.");
		return false;
	}

	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		ai_log_error("Execute 'remove_autoload_singleton': ProjectSettings singleton not available.");
		return false;
	}

	if (!args.has("name") || args["name"].get_type() != Variant::STRING) {
		ai_log_error("Execute 'remove_autoload_singleton': 'name' must be a string.");
		return false;
	}

	String name = args["name"];
	String autoload_key = "autoload/" + name;

	// Check if autoload exists
	if (!ps->has_setting(autoload_key)) {
		ai_log_error(vformat("Execute 'remove_autoload_singleton': Autoload '%s' does not exist.", name));
		return false;
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

	print_line(vformat("AI: Executed remove_autoload_singleton. Name: %s", name));
	return true;
#else
	ai_log_error("Execute 'remove_autoload_singleton': Editor API not available in non-editor builds.");
	return false;
#endif
}

bool exec_import_asset(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	EditorUndoRedoManager *undo_redo = ai_get_undo_redo();
	if (!undo_redo) {
		ai_log_error("Execute 'import_asset': EditorUndoRedoManager singleton not found.");
		return false;
	}

	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		ai_log_error("Execute 'import_asset': ProjectSettings singleton not available.");
		return false;
	}

	AI *ai_singleton = AI::get_singleton();
	if (!ai_singleton) {
		ai_log_error("Execute 'import_asset': AI singleton not found.");
		return false;
	}

	if (!args.has("source_path") || args["source_path"].get_type() != Variant::STRING) {
		ai_log_error("Execute 'import_asset': 'source_path' must be a string.");
		return false;
	}
	if (!args.has("dest_path") || args["dest_path"].get_type() != Variant::STRING) {
		ai_log_error("Execute 'import_asset': 'dest_path' must be a string.");
		return false;
	}

	String source_path = args["source_path"];
	String dest_path = args["dest_path"];
	bool overwrite = args.get("overwrite", false);

	// Validate source file exists
	if (!FileAccess::exists(source_path)) {
		ai_log_error(vformat("Execute 'import_asset': Source file does not exist at path '%s'.", source_path));
		return false;
	}

	// Convert dest_path to absolute OS path
	String dest_abs_path = ps->globalize_path(dest_path);

	// Check if dest exists
	bool dest_exists = FileAccess::exists(dest_abs_path);
	if (dest_exists && !overwrite) {
		ai_log_error(vformat("Execute 'import_asset': Destination file already exists at '%s' and overwrite is false.", dest_path));
		return false;
	}

	// Read source file
	PackedByteArray source_bytes = FileAccess::get_file_as_bytes(source_path);
	if (source_bytes.is_empty()) {
		ai_log_error(vformat("Execute 'import_asset': Source file at '%s' is empty or could not be read.", source_path));
		return false;
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
			ai_log_error(vformat("Execute 'import_asset': Failed to create destination directory '%s'. Error: %d", dest_dir, dir_err));
			return false;
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

	print_line(vformat("AI: Executed import_asset. Source: %s, Dest: %s", source_path, dest_path));
	return true;
#else
	ai_log_error("Execute 'import_asset': Editor API not available in non-editor builds.");
	return false;
#endif
}

bool exec_delete_asset(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	EditorUndoRedoManager *undo_redo = ai_get_undo_redo();
	if (!undo_redo) {
		ai_log_error("Execute 'delete_asset': EditorUndoRedoManager singleton not found.");
		return false;
	}

	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		ai_log_error("Execute 'delete_asset': ProjectSettings singleton not available.");
		return false;
	}

	AI *ai_singleton = AI::get_singleton();
	if (!ai_singleton) {
		ai_log_error("Execute 'delete_asset': AI singleton not found.");
		return false;
	}

	if (!args.has("asset_path") || args["asset_path"].get_type() != Variant::STRING) {
		ai_log_error("Execute 'delete_asset': 'asset_path' must be a string.");
		return false;
	}

	String asset_path = args["asset_path"];

	// Convert asset_path to absolute OS path
	String asset_abs_path = ps->globalize_path(asset_path);

	// Check if file exists
	if (!FileAccess::exists(asset_abs_path)) {
		ai_log_error(vformat("Execute 'delete_asset': Asset file does not exist at path '%s'.", asset_path));
		return false;
	}

	// Read old bytes for undo
	PackedByteArray old_bytes = FileAccess::get_file_as_bytes(asset_abs_path);

	ai_log_verbose(vformat("Deleting asset at '%s'", asset_path));

	// Wrap in UndoRedo
	undo_redo->create_action("AI Delete Asset");
	undo_redo->add_do_method(ai_singleton, "_delete_script_file", asset_abs_path);
	undo_redo->add_undo_method(ai_singleton, "_write_binary_file", asset_abs_path, old_bytes);
	undo_redo->commit_action();

	print_line(vformat("AI: Executed delete_asset. Path: %s", asset_path));
	return true;
#else
	ai_log_error("Execute 'delete_asset': Editor API not available in non-editor builds.");
	return false;
#endif
}

} // namespace AIProjectActions

