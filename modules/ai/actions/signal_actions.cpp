// modules/ai/actions/signal_actions.cpp
// Signal-related action implementations for the AI module

#include "signal_actions.h"
#include "action_common.h"

#include "core/variant/callable.h"
#include "core/variant/array.h"
#include "core/string/string_name.h"
#include "scene/main/node.h"
#include "core/object/object.h"

namespace AISignalActions {

bool exec_connect_signal(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	EditorUndoRedoManager *undo_redo = ai_get_undo_redo();
	if (!undo_redo) {
		ai_log_error("Execute 'connect_signal': EditorUndoRedoManager singleton not found.");
		return false;
	}

	Node *edited_scene_root = ai_get_edited_scene_root();
	if (!edited_scene_root) {
		ai_log_error("Execute 'connect_signal': No edited scene root.");
		return false;
	}

	// Validate required arguments
	if (!args.has("emitter_path") || args["emitter_path"].get_type() != Variant::STRING) {
		ai_log_error("Execute 'connect_signal': 'emitter_path' must be a string.");
		return false;
	}
	if (!args.has("signal_name") || args["signal_name"].get_type() != Variant::STRING) {
		ai_log_error("Execute 'connect_signal': 'signal_name' must be a string.");
		return false;
	}
	if (!args.has("target_path") || args["target_path"].get_type() != Variant::STRING) {
		ai_log_error("Execute 'connect_signal': 'target_path' must be a string.");
		return false;
	}
	if (!args.has("method_name") || args["method_name"].get_type() != Variant::STRING) {
		ai_log_error("Execute 'connect_signal': 'method_name' must be a string.");
		return false;
	}

	String emitter_path_str = args["emitter_path"];
	String signal_name_str = args["signal_name"];
	String target_path_str = args["target_path"];
	String method_name_str = args["method_name"];

	// Resolve emitter node
	Node *emitter = ai_get_node_by_path(emitter_path_str);
	if (!emitter) {
		ai_log_error(vformat("Execute 'connect_signal': Could not find emitter node at path '%s'.", emitter_path_str));
		return false;
	}

	// Resolve target object (can be any Object, not just Node)
	Object *target = nullptr;
	if (target_path_str.is_empty() || target_path_str == edited_scene_root->get_name()) {
		target = edited_scene_root;
	} else {
		Node *target_node = ai_get_node_by_path(target_path_str);
		if (!target_node) {
			ai_log_error(vformat("Execute 'connect_signal': Could not find target node at path '%s'.", target_path_str));
			return false;
		}
		target = target_node;
	}

	if (!target) {
		ai_log_error("Execute 'connect_signal': Target object is null.");
		return false;
	}

	// Build Callable
	StringName signal_name = StringName(signal_name_str);
	StringName method_name = StringName(method_name_str);
	Callable callable(target, method_name);

	// Apply binds if provided
	if (args.has("binds") && args["binds"].get_type() == Variant::ARRAY) {
		Array binds = args["binds"];
		if (!binds.is_empty()) {
			callable = callable.bindv(binds);
		}
	}

	// Get flags (default to 0)
	uint32_t flags = 0;
	if (args.has("flags") && args["flags"].get_type() == Variant::INT) {
		flags = (uint32_t)args["flags"];
	}

	// Check if already connected
	if (emitter->is_connected(signal_name, callable)) {
		ai_log_error(vformat("Execute 'connect_signal': Signal '%s' is already connected to '%s::%s'.", signal_name_str, target_path_str, method_name_str));
		return false;
	}

	ai_log_verbose(vformat("Connecting signal '%s' from '%s' to '%s::%s'", signal_name_str, emitter_path_str, target_path_str, method_name_str));

	// Wrap in UndoRedo
	undo_redo->create_action("AI Connect Signal");
	undo_redo->add_do_method(emitter, "connect", signal_name, callable, flags);
	undo_redo->add_undo_method(emitter, "disconnect", signal_name, callable);
	undo_redo->commit_action();

	print_line(vformat("AI: Executed connect_signal. Emitter: %s, Signal: %s, Target: %s, Method: %s", emitter_path_str, signal_name_str, target_path_str, method_name_str));
	return true;
#else
	ai_log_error("Execute 'connect_signal': Editor API not available in non-editor builds.");
	return false;
#endif
}

bool exec_disconnect_signal(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	EditorUndoRedoManager *undo_redo = ai_get_undo_redo();
	if (!undo_redo) {
		ai_log_error("Execute 'disconnect_signal': EditorUndoRedoManager singleton not found.");
		return false;
	}

	Node *edited_scene_root = ai_get_edited_scene_root();
	if (!edited_scene_root) {
		ai_log_error("Execute 'disconnect_signal': No edited scene root.");
		return false;
	}

	// Validate required arguments
	if (!args.has("emitter_path") || args["emitter_path"].get_type() != Variant::STRING) {
		ai_log_error("Execute 'disconnect_signal': 'emitter_path' must be a string.");
		return false;
	}
	if (!args.has("signal_name") || args["signal_name"].get_type() != Variant::STRING) {
		ai_log_error("Execute 'disconnect_signal': 'signal_name' must be a string.");
		return false;
	}
	if (!args.has("target_path") || args["target_path"].get_type() != Variant::STRING) {
		ai_log_error("Execute 'disconnect_signal': 'target_path' must be a string.");
		return false;
	}
	if (!args.has("method_name") || args["method_name"].get_type() != Variant::STRING) {
		ai_log_error("Execute 'disconnect_signal': 'method_name' must be a string.");
		return false;
	}

	String emitter_path_str = args["emitter_path"];
	String signal_name_str = args["signal_name"];
	String target_path_str = args["target_path"];
	String method_name_str = args["method_name"];

	// Resolve emitter node
	Node *emitter = ai_get_node_by_path(emitter_path_str);
	if (!emitter) {
		ai_log_error(vformat("Execute 'disconnect_signal': Could not find emitter node at path '%s'.", emitter_path_str));
		return false;
	}

	// Resolve target object (can be any Object, not just Node)
	Object *target = nullptr;
	if (target_path_str.is_empty() || target_path_str == edited_scene_root->get_name()) {
		target = edited_scene_root;
	} else {
		Node *target_node = ai_get_node_by_path(target_path_str);
		if (!target_node) {
			ai_log_error(vformat("Execute 'disconnect_signal': Could not find target node at path '%s'.", target_path_str));
			return false;
		}
		target = target_node;
	}

	if (!target) {
		ai_log_error("Execute 'disconnect_signal': Target object is null.");
		return false;
	}

	// Build Callable
	StringName signal_name = StringName(signal_name_str);
	StringName method_name = StringName(method_name_str);
	Callable callable(target, method_name);

	// Check if connected - if not, return true (idempotent)
	if (!emitter->is_connected(signal_name, callable)) {
		ai_log_verbose(vformat("Execute 'disconnect_signal': Signal '%s' is not connected to '%s::%s'. Already disconnected.", signal_name_str, target_path_str, method_name_str));
		return true;
	}

	ai_log_verbose(vformat("Disconnecting signal '%s' from '%s' to '%s::%s'", signal_name_str, emitter_path_str, target_path_str, method_name_str));

	// Wrap in UndoRedo
	// Note: For undo, we reconnect with default flags (0) since we don't have the original flags
	undo_redo->create_action("AI Disconnect Signal");
	undo_redo->add_do_method(emitter, "disconnect", signal_name, callable);
	undo_redo->add_undo_method(emitter, "connect", signal_name, callable, 0);
	undo_redo->commit_action();

	print_line(vformat("AI: Executed disconnect_signal. Emitter: %s, Signal: %s, Target: %s, Method: %s", emitter_path_str, signal_name_str, target_path_str, method_name_str));
	return true;
#else
	ai_log_error("Execute 'disconnect_signal': Editor API not available in non-editor builds.");
	return false;
#endif
}

} // namespace AISignalActions

