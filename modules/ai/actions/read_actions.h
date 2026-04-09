// modules/ai/actions/read_actions.h
// Read-only, introspection-related action implementations for the AI module

#ifndef AI_READ_ACTIONS_H
#define AI_READ_ACTIONS_H

#include "core/variant/dictionary.h"

namespace AIReadActions {

// Lists nodes in the current scene tree.
// Optional args: root_path (String) - if omitted, uses edited scene root;
// if provided, lists that node's subtree recursively.
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_list_nodes(const Dictionary &args);

// Gets detailed information about a single node.
// Required args: node_path (String)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_get_node_info(const Dictionary &args);

// Finds all nodes of a specific type in the scene tree.
// Required args: type_name (String)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_find_nodes_by_type(const Dictionary &args);

// Lists files in a directory.
// Required args: directory (String)
// Optional args: glob (String) - e.g. "*.gd" for simple suffix filtering
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_list_files(const Dictionary &args);

// Reads the source content of an existing script file.
// Required args: file_path (String) - must start with "res://"
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_read_script(const Dictionary &args);

// Returns low-res image previews as base64 PNG thumbnails.
// Required args: paths (Array of Strings) - res:// image paths
// Optional args: max_size (int) - max dimension in px (default 128)
// Returns: Dictionary with previews array and _images array for vision
Dictionary exec_preview_asset(const Dictionary &args);

} // namespace AIReadActions

#endif // AI_READ_ACTIONS_H