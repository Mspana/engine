// modules/ai/actions/action_common.h
// Shared utility functions for AI action implementations

#ifndef AI_ACTION_COMMON_H
#define AI_ACTION_COMMON_H

#include "core/error/error_macros.h"
#include "core/string/ustring.h"
#include "core/variant/dictionary.h"
#include "editor/editor_interface.h"
#include "editor/editor_undo_redo_manager.h"
#include "scene/main/node.h"

// Returns the EditorUndoRedoManager singleton, or nullptr if unavailable.
static inline EditorUndoRedoManager *ai_get_undo_redo() {
	return EditorUndoRedoManager::get_singleton();
}

// Returns the currently edited scene root, or nullptr if unavailable.
static inline Node *ai_get_edited_scene_root() {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return nullptr;
	}
	return ei->get_edited_scene_root();
}

// Resolves a node path relative to the edited scene root.
// Returns nullptr if the scene root or node is not found.
static inline Node *ai_get_node_by_path(const String &node_path) {
	Node *root = ai_get_edited_scene_root();
	if (!root) {
		return nullptr;
	}
	
	// Handle empty path or root reference
	if (node_path.is_empty()) {
		return root;
	}
	
	// Check if path matches the scene root's name
	if (node_path == root->get_name()) {
		return root;
	}
	
	// Strip leading slash if present (absolute paths don't work from scene root)
	String relative_path = node_path;
	if (relative_path.begins_with("/")) {
		relative_path = relative_path.substr(1);
	}
	
	return root->get_node_or_null(NodePath(relative_path));
}

// Returns true if the given node is the edited scene root.
static inline bool ai_is_scene_root(Node *n) {
	Node *root = ai_get_edited_scene_root();
	return root != nullptr && n == root;
}

// Verbose logging wrapper for AI actions.
static inline void ai_log_verbose(const String &msg) {
	print_verbose(vformat("AI: %s", msg));
}

// Error logging wrapper for AI actions.
static inline void ai_log_error(const String &msg) {
	ERR_PRINT(vformat("AI: %s", msg));
}

// === Result Builder Helpers for Agentic Tool Use ===

// Create a success result with optional result data
static inline Dictionary ai_create_success_result(const Dictionary &p_result_data = Dictionary()) {
	Dictionary result;
	result["status"] = "success";
	result["result"] = p_result_data;
	return result;
}

// Create an error result with error code, message, and optional details
static inline Dictionary ai_create_error_result(const String &p_error_code, const String &p_error_message, const Dictionary &p_details = Dictionary()) {
	Dictionary result;
	result["status"] = "error";

	Dictionary error_dict;
	error_dict["code"] = p_error_code;
	error_dict["message"] = p_error_message;
	error_dict["details"] = p_details;

	result["error"] = error_dict;
	return result;
}

// Common error codes (for consistency)
namespace AIErrorCodes {
	static const char *NO_UNDO_REDO = "no_undo_redo";
	static const char *NO_ACTIVE_SCENE = "no_active_scene";
	static const char *NODE_NOT_FOUND = "node_not_found";
	static const char *FILE_NOT_FOUND = "file_not_found";
	static const char *INVALID_PATH = "invalid_path";
	static const char *INVALID_TYPE = "invalid_type";
	static const char *INVALID_ARGS = "invalid_args";
	static const char *OPERATION_FAILED = "operation_failed";
	static const char *INTERNAL_ERROR = "internal_error";
}

#endif // AI_ACTION_COMMON_H

