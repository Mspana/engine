// modules/ai/actions/action_common.h
// Shared utility functions for AI action implementations

#ifndef AI_ACTION_COMMON_H
#define AI_ACTION_COMMON_H

#include "core/error/error_macros.h"
#include "core/string/ustring.h"
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
	
	return root->get_node_or_null(NodePath(node_path));
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

#endif // AI_ACTION_COMMON_H

