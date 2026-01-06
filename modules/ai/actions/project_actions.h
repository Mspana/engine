// modules/ai/actions/project_actions.h
// Project settings-related action implementations for the AI module

#ifndef AI_PROJECT_ACTIONS_H
#define AI_PROJECT_ACTIONS_H

#include "core/variant/dictionary.h"

namespace AIProjectActions {

// Sets a project setting value.
// Required args: key (String), value (Variant)
bool exec_set_project_setting(const Dictionary &args);

// Gets project settings (read-only).
// Optional args: prefix (String), keys (Array[String]), include_defaults (bool, default false)
bool exec_get_project_settings(const Dictionary &args);

// Creates an autoload singleton entry.
// Required args: name (String), script_path (String)
// Optional args: enabled (bool, default true)
bool exec_create_autoload_singleton(const Dictionary &args);

// Removes an autoload singleton entry.
// Required args: name (String)
bool exec_remove_autoload_singleton(const Dictionary &args);

// Imports an asset file from OS path to project path.
// Required args: source_path (String, absolute OS path), dest_path (String, res://...)
// Optional args: overwrite (bool, default false)
bool exec_import_asset(const Dictionary &args);

// Deletes an asset file from the project.
// Required args: asset_path (String, res://...)
bool exec_delete_asset(const Dictionary &args);

} // namespace AIProjectActions

#endif // AI_PROJECT_ACTIONS_H

