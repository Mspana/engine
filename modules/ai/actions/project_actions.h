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

} // namespace AIProjectActions

#endif // AI_PROJECT_ACTIONS_H

