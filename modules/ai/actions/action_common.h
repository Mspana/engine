// modules/ai/actions/action_common.h
// Shared utility functions for AI action implementations

#ifndef AI_ACTION_COMMON_H
#define AI_ACTION_COMMON_H

#include "core/error/error_macros.h"
#include "core/string/ustring.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "core/variant/typed_array.h"
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

// Looks up an *already-open* scene by its file path (e.g. "res://main.tscn") among
// the editor's currently open scene tabs and returns its root node. Returns nullptr
// if no open tab matches. Does NOT load scenes from disk — the caller must open the
// scene first if they want to inspect it. This avoids the side-effects (autoloads,
// resource refcount, dirty markers) of an implicit on-demand scene load.
static inline Node *ai_get_open_scene_root_by_path(const String &p_scene_path) {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei || p_scene_path.is_empty()) {
		return nullptr;
	}
	PackedStringArray paths = ei->get_open_scenes();
	TypedArray<Node> roots = ei->get_open_scene_roots();
	const int n = MIN(paths.size(), roots.size());
	for (int i = 0; i < n; i++) {
		if (paths[i] == p_scene_path) {
			Object *obj = roots[i];
			return Object::cast_to<Node>(obj);
		}
	}
	return nullptr;
}

// Reads `args["scene_path"]` (optional). If empty/absent, returns the currently edited
// scene root. If non-empty, looks up the matching open scene tab. Returns nullptr if
// the requested scene isn't open — caller should emit an error pointing the user at
// the available open scenes.
static inline Node *ai_resolve_scene_root_from_args(const Dictionary &args) {
	String scene_path = args.get("scene_path", String());
	if (scene_path.is_empty()) {
		return ai_get_edited_scene_root();
	}
	return ai_get_open_scene_root_by_path(scene_path);
}

// Resolves a node path relative to a given root, applying the same forgiveness rules
// as ai_get_node_by_path. Returns nullptr if root is null or no descendant matches.
static inline Node *ai_get_node_by_path_in_root(Node *root, const String &node_path) {
	if (!root) {
		return nullptr;
	}

	// Handle empty path
	if (node_path.is_empty()) {
		return root;
	}

	// Strip leading slash if present
	String relative_path = node_path;
	if (relative_path.begins_with("/")) {
		relative_path = relative_path.substr(1);
	}

	// Exact match on root name
	String root_name = root->get_name();
	if (relative_path == root_name) {
		return root;
	}

	// Direct lookup (normal case: "Player", "Player/Weapon", etc.)
	Node *result = root->get_node_or_null(NodePath(relative_path));
	if (result) {
		return result;
	}

	// Forgiveness: path may include root name as prefix, e.g. "Main/Player" when root is "Main".
	if (relative_path.begins_with(root_name + "/")) {
		String stripped = relative_path.substr(root_name.length() + 1);
		result = root->get_node_or_null(NodePath(stripped));
		if (result) {
			return result;
		}
	}

	return nullptr;
}

// Resolves a node path relative to the edited scene root.
// Returns nullptr if the scene root or node is not found.
// Forgiveness: if the path begins with the scene root's own name (e.g. "Main/Player"
// when root is "Main"), strips that prefix and retries with "Player".
static inline Node *ai_get_node_by_path(const String &node_path) {
	return ai_get_node_by_path_in_root(ai_get_edited_scene_root(), node_path);
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

// Returns a Variant Array of configuration warning strings for a node.
// Empty array = no warnings (node is properly configured).
static inline Array ai_get_node_warnings(Node *p_node) {
	Array result;
	if (!p_node) {
		return result;
	}
	PackedStringArray warnings = p_node->get_configuration_warnings();
	for (int i = 0; i < warnings.size(); i++) {
		result.push_back(warnings[i]);
	}
	return result;
}

// Builds the standard "scene not open" error result for tools that take an optional
// scene_path. Lists currently open scenes so the model can self-correct.
static inline Dictionary ai_scene_not_open_error(const String &p_requested_path) {
	EditorInterface *ei = EditorInterface::get_singleton();
	PackedStringArray open_paths = ei ? ei->get_open_scenes() : PackedStringArray();
	Dictionary details;
	details["requested_scene_path"] = p_requested_path;
	details["open_scenes"] = open_paths;
	details["hint"] = "scene_path must match the file path of a currently open scene tab. Open the scene first (open_scene tool) or omit scene_path to use the active scene.";
	return ai_create_error_result("no_active_scene",
		vformat("Scene '%s' is not open in the editor.", p_requested_path),
		details);
}

// Create a node-not-found error with scene root context to help the model self-correct.
static inline Dictionary ai_node_not_found_error(const String &tried_path) {
	Node *root = ai_get_edited_scene_root();
	String root_name = root ? String(root->get_name()) : "unknown";
	Dictionary details;
	details["tried_path"] = tried_path;
	details["scene_root"] = root_name;
	details["hint"] = vformat("Paths are relative to the scene root. Use '%s' to refer to the root, or 'Child' / 'Child/Grandchild' for descendants. Never include the root name as a prefix (e.g. use 'Player' not '%s/Player').", root_name, root_name);
	return ai_create_error_result("node_not_found",
		vformat("Could not find node at path '%s'. Scene root is '%s'. Try '%s' instead of '%s/%s'.",
			tried_path, root_name,
			tried_path.begins_with(root_name + "/") ? tried_path.substr(root_name.length() + 1) : tried_path.get_slice("/", tried_path.get_slice_count("/") - 1),
			root_name, tried_path.begins_with(root_name + "/") ? tried_path.substr(root_name.length() + 1) : tried_path),
		details);
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

// Resolves the scene a MUTATION tool should target from the optional args["scene_path"],
// switching the focused scene tab when necessary. Mutation tools must operate on the
// focused scene: EditorUndoRedoManager routes a node's undo history through the
// *currently edited* scene, so mutating a background tab would push the action into the
// global history and skip that tab's dirty-marking.
//   - scene_path absent/empty → the currently edited scene (error if none).
//   - scene_path == the focused scene → no-op.
//   - scene_path is another OPEN tab → switches focus to it (synchronous).
//   - scene_path not open → error; the model should call open_scene first.
// Returns the target scene root, or nullptr with r_error filled.
// r_switched_tab (optional) reports whether a tab switch happened, so tools can
// surface it in their result.
static inline Node *ai_focus_scene_for_mutation(const Dictionary &args, Dictionary &r_error, bool *r_switched_tab = nullptr) {
	if (r_switched_tab) {
		*r_switched_tab = false;
	}
	String scene_path = args.get("scene_path", String());
	if (scene_path.is_empty()) {
		Node *current = ai_get_edited_scene_root();
		if (!current) {
			r_error = ai_create_error_result(AIErrorCodes::NO_ACTIVE_SCENE,
				"No edited scene root");
		}
		return current;
	}
	Node *current = ai_get_edited_scene_root();
	if (current && current->get_scene_file_path() == scene_path) {
		return current;
	}
	Node *open_root = ai_get_open_scene_root_by_path(scene_path);
	if (!open_root) {
		r_error = ai_scene_not_open_error(scene_path);
		return nullptr;
	}
	// open_scene_from_path on an already-open path switches tabs synchronously,
	// unless the editor is mid scene-change — verify the switch actually landed.
	EditorInterface::get_singleton()->open_scene_from_path(scene_path);
	if (ai_get_edited_scene_root() != open_root) {
		r_error = ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("Could not switch to scene tab '%s' (the editor is busy changing scenes). Retry, or call open_scene first.", scene_path));
		return nullptr;
	}
	if (r_switched_tab) {
		*r_switched_tab = true;
	}
	return open_root;
}

#endif // AI_ACTION_COMMON_H

