// modules/ai/actions/read_actions.h
// Read-only, introspection-related action implementations for the AI module

#ifndef AI_READ_ACTIONS_H
#define AI_READ_ACTIONS_H

#include "core/variant/dictionary.h"

namespace AIReadActions {

// Lists nodes in the current scene tree.
// Optional args: root_path (String) - if omitted, uses edited scene root;
// if provided, lists that node's subtree recursively.
bool exec_list_nodes(const Dictionary &args);

} // namespace AIReadActions

#endif // AI_READ_ACTIONS_H