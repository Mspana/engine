// modules/ai/actions/script_actions.h
// Script-related action implementations for the AI module

#ifndef AI_SCRIPT_ACTIONS_H
#define AI_SCRIPT_ACTIONS_H

#include "core/variant/dictionary.h"

namespace AIScriptActions {

// Creates a new script file.
// Required args: file_path (String), language (String), content (String)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_create_script(const Dictionary &args);

// Updates an existing script file with new content.
// Required args: file_path (String), patch (String)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_update_script(const Dictionary &args);

// Attaches a script to an existing node.
// Required args: node_path (String), script_path (String)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_attach_script(const Dictionary &args);

// Detaches a script from an existing node.
// Required args: node_path (String)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_detach_script(const Dictionary &args);

// Renames/moves a script file.
// Required args: old_path (String), new_path (String)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_rename_script(const Dictionary &args);

// Deletes a script file.
// Required args: file_path (String)
// Optional args: detach_from_nodes (bool, default false)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_delete_script(const Dictionary &args);

} // namespace AIScriptActions

#endif // AI_SCRIPT_ACTIONS_H

