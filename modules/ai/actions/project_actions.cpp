// modules/ai/actions/project_actions.cpp
// Project settings-related action implementations for the AI module

#include "project_actions.h"
#include "action_common.h"

#include "core/config/project_settings.h"
#include "core/variant/variant.h"
#include "core/variant/array.h"
#include "core/object/object.h"

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

} // namespace AIProjectActions

