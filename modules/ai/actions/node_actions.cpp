// modules/ai/actions/node_actions.cpp
// Node-related action implementations for the AI module

#include "node_actions.h"
#include "action_common.h"

#include "core/object/class_db.h"
#include "core/string/node_path.h"
#include "core/io/json.h"
#include "scene/main/node.h"

namespace AINodeActions {

bool exec_create_node(const Dictionary &args) {
	EditorUndoRedoManager *undo_redo = ai_get_undo_redo();
	if (!undo_redo) {
		ai_log_error("Execute 'create_node': EditorUndoRedoManager singleton not found.");
		return false;
	}

	Node *edited_scene_root = ai_get_edited_scene_root();
	if (!edited_scene_root) {
		ai_log_error("Execute 'create_node': No edited scene root.");
		return false;
	}

	String node_name = args["node_name"];
	String node_type = args["node_type"];
	String parent_path_str = args.get("parent_path", "");

	if (!ClassDB::class_exists(StringName(node_type))) {
		ai_log_error(vformat("Execute 'create_node': Node type '%s' does not exist.", node_type));
		return false;
	}

	Node *parent_node = nullptr;
	if (parent_path_str.is_empty()) {
		parent_node = edited_scene_root;
	} else {
		// Check if parent_path matches the scene root's name
		if (parent_path_str == edited_scene_root->get_name()) {
			parent_node = edited_scene_root;
		} else {
			parent_node = edited_scene_root->get_node_or_null(NodePath(parent_path_str));
			if (!parent_node) {
				ai_log_error(vformat("Execute 'create_node': Could not find parent node at path '%s'. Using scene root instead.", parent_path_str));
				parent_node = edited_scene_root;
			}
		}
	}

	if (!parent_node) {
		ai_log_error("Execute 'create_node': Failed to determine parent node.");
		return false;
	}

	Node *new_node = Object::cast_to<Node>(ClassDB::instantiate(StringName(node_type)));
	if (!new_node) {
		ai_log_error(vformat("Execute 'create_node': Failed to instantiate node of type '%s'.", node_type));
		return false;
	}

	new_node->set_name(node_name);

	// Set up undo/redo - commit_action() will execute do_methods immediately
	undo_redo->create_action("AI Create Node");
	undo_redo->add_do_method(parent_node, "add_child", new_node, true); // force_readable_name = true
	undo_redo->add_do_method(new_node, "set_owner", edited_scene_root);
	undo_redo->add_do_reference(new_node); // Keep reference during undo/redo
	undo_redo->add_undo_method(parent_node, "remove_child", new_node);
	undo_redo->add_undo_method(new_node, "queue_free");
	undo_redo->commit_action();

	print_line(vformat("AI: Executed create_node. Name: %s, Type: %s, Parent: %s", node_name, node_type, parent_node->get_path()));
	return true;
}

bool exec_set_property(const Dictionary &args) {
	EditorUndoRedoManager *undo_redo = ai_get_undo_redo();
	if (!undo_redo) {
		ai_log_error("Execute 'set_property': EditorUndoRedoManager singleton not found.");
		return false;
	}

	Node *edited_scene_root = ai_get_edited_scene_root();
	if (!edited_scene_root) {
		ai_log_error("Execute 'set_property': No edited scene root.");
		return false;
	}

	String node_path_str = args["node_path"];
	String property_name = args["property_name"];
	Variant value = args["value"];

	Node *target_node = edited_scene_root->get_node_or_null(NodePath(node_path_str));
	if (!target_node) {
		ai_log_error(vformat("Execute 'set_property': Could not find node at path '%s'.", node_path_str));
		return false;
	}

	Variant current_value = target_node->get(property_name);

	undo_redo->create_action("AI Set Property");
	undo_redo->add_do_method(target_node, "set", property_name, value);
	undo_redo->add_undo_method(target_node, "set", property_name, current_value);
	undo_redo->commit_action();

	print_line(vformat("AI: Executed set_property. Node: %s, Property: %s, Value: %s", node_path_str, property_name, String(value)));
	return true;
}

bool exec_rename_node(const Dictionary &args) {
	EditorUndoRedoManager *undo_redo = ai_get_undo_redo();
	if (!undo_redo) {
		ai_log_error("Execute 'rename_node': EditorUndoRedoManager singleton not found.");
		return false;
	}

	Node *edited_scene_root = ai_get_edited_scene_root();
	if (!edited_scene_root) {
		ai_log_error("Execute 'rename_node': No edited scene root.");
		return false;
	}

	String node_path_str = args["node_path"];
	String new_name = args["new_name"];

	if (new_name.is_empty()) {
		ai_log_error("Execute 'rename_node': new_name cannot be empty.");
		return false;
	}

	Node *target = edited_scene_root->get_node_or_null(NodePath(node_path_str));
	if (!target) {
		ai_log_error(vformat("Execute 'rename_node': Could not find node at path '%s'.", node_path_str));
		return false;
	}

	String old_name = target->get_name();

	ai_log_verbose(vformat("Renaming node '%s' from '%s' to '%s'", node_path_str, old_name, new_name));

	undo_redo->create_action("AI Rename Node");
	undo_redo->add_do_method(target, "set_name", new_name);
	undo_redo->add_undo_method(target, "set_name", old_name);
	undo_redo->commit_action();

	print_line(vformat("AI: Executed rename_node. Path: %s, OldName: %s, NewName: %s", node_path_str, old_name, new_name));
	return true;
}

} // namespace AINodeActions

