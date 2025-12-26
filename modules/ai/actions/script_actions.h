// modules/ai/actions/script_actions.h
// Script-related action implementations for the AI module

#ifndef AI_SCRIPT_ACTIONS_H
#define AI_SCRIPT_ACTIONS_H

#include "core/variant/dictionary.h"

namespace AIScriptActions {

// Creates a new script file.
// Required args: file_path (String), language (String), content (String)
bool exec_create_script(const Dictionary &args);

// Updates an existing script file with new content.
// Required args: file_path (String), patch (String)
bool exec_update_script(const Dictionary &args);

// Attaches a script to an existing node.
// Required args: node_path (String), script_path (String)
bool exec_attach_script(const Dictionary &args);

} // namespace AIScriptActions

#endif // AI_SCRIPT_ACTIONS_H

