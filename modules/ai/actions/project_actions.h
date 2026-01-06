// modules/ai/actions/project_actions.h
// Project settings-related action implementations for the AI module

#ifndef AI_PROJECT_ACTIONS_H
#define AI_PROJECT_ACTIONS_H

#include "core/variant/dictionary.h"

namespace AIProjectActions {

// Sets a project setting value.
// Required args: key (String), value (Variant)
bool exec_set_project_setting(const Dictionary &args);

} // namespace AIProjectActions

#endif // AI_PROJECT_ACTIONS_H

