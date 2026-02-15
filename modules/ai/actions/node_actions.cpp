// modules/ai/actions/node_actions.cpp
// Node-related action implementations for the AI module

#include "node_actions.h"
#include "action_common.h"

#include "core/object/class_db.h"
#include "core/string/node_path.h"
#include "core/io/json.h"
#include "scene/main/node.h"

namespace AINodeActions {

Dictionary exec_create_node(const Dictionary &args) {
	EditorUndoRedoManager *undo_redo = ai_get_undo_redo();
	if (!undo_redo) {
		return ai_create_error_result(AIErrorCodes::NO_UNDO_REDO,
			"EditorUndoRedoManager singleton not found");
	}

	Node *edited_scene_root = ai_get_edited_scene_root();
	if (!edited_scene_root) {
		return ai_create_error_result(AIErrorCodes::NO_ACTIVE_SCENE,
			"No edited scene root");
	}

	String node_name = args["node_name"];
	String node_type = args["node_type"];
	String parent_path_str = args.get("parent_path", "");

	if (!ClassDB::class_exists(StringName(node_type))) {
		return ai_create_error_result(AIErrorCodes::INVALID_TYPE,
			vformat("Node type '%s' does not exist", node_type));
	}

	Node *parent_node = nullptr;
	if (parent_path_str.is_empty()) {
		parent_node = edited_scene_root;
	} else {
		parent_node = ai_get_node_by_path(parent_path_str);
		if (!parent_node) {
			return ai_node_not_found_error(parent_path_str);
		}
	}

	if (!parent_node) {
		return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			"Failed to determine parent node");
	}

	Node *new_node = Object::cast_to<Node>(ClassDB::instantiate(StringName(node_type)));
	if (!new_node) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("Failed to instantiate node of type '%s'", node_type));
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

	// Return success with details
	Dictionary result_data;
	result_data["node_name"] = node_name;
	result_data["node_type"] = node_type;
	result_data["parent_path"] = parent_node->get_path();
	result_data["node_path"] = new_node->get_path();
	result_data["warnings"] = ai_get_node_warnings(new_node);
	result_data["parent_warnings"] = ai_get_node_warnings(parent_node);

	print_line(vformat("AI: Executed create_node. Name: %s, Type: %s, Parent: %s", node_name, node_type, parent_node->get_path()));
	return ai_create_success_result(result_data);
}

Dictionary exec_set_property(const Dictionary &args) {
	EditorUndoRedoManager *undo_redo = ai_get_undo_redo();
	if (!undo_redo) {
		return ai_create_error_result(AIErrorCodes::NO_UNDO_REDO,
			"EditorUndoRedoManager singleton not found");
	}

	Node *edited_scene_root = ai_get_edited_scene_root();
	if (!edited_scene_root) {
		return ai_create_error_result(AIErrorCodes::NO_ACTIVE_SCENE,
			"No edited scene root");
	}

	String node_path_str = args["node_path"];
	String property_name = args["property_name"];
	Variant value = args["value"];

	Node *target_node = ai_get_node_by_path(node_path_str);
	if (!target_node) {
		return ai_node_not_found_error(node_path_str);
	}

	if (!property_name.contains(".")) {
		// Flat property path — set directly on the node.
		Variant current_value = target_node->get(property_name);

		undo_redo->create_action("AI Set Property");
		undo_redo->add_do_method(target_node, "set", property_name, value);
		undo_redo->add_undo_method(target_node, "set", property_name, current_value);
		undo_redo->commit_action();

		Dictionary result_data;
		result_data["node_path"] = node_path_str;
		result_data["property_name"] = property_name;
		result_data["old_value"] = current_value;
		result_data["new_value"] = value;
		result_data["warnings"] = ai_get_node_warnings(target_node);

		print_line(vformat("AI: Executed set_property. Node: %s, Property: %s, Value: %s", node_path_str, property_name, String(value)));
		return ai_create_success_result(result_data);
	} else {
		// Dot-notation sub-resource path, e.g. "mesh.size" or "mat.albedo_color".
		// Walk all segments except the last, traversing the resource chain.
		PackedStringArray segs = property_name.split(".");

		Object *cur = target_node;
		for (int i = 0; i < segs.size() - 1; i++) {
			Variant v = cur->get(segs[i]);
			if (v.get_type() != Variant::OBJECT) {
				return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
					vformat("Property '%s' is not a resource (got type %s). Assign a resource first.",
						segs[i], Variant::get_type_name(v.get_type())));
			}
			Object *obj = v.operator Object *();
			if (!obj) {
				return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
					vformat("Property '%s' is null. Use create_resource to assign a resource first.", segs[i]));
			}
			cur = obj;
		}

		String final_prop = segs[segs.size() - 1];
		Variant old_value = cur->get(final_prop);

		undo_redo->create_action("AI Set Property");
		undo_redo->add_do_method(cur, "set", final_prop, value);
		undo_redo->add_undo_method(cur, "set", final_prop, old_value);
		undo_redo->commit_action();

		Dictionary result_data;
		result_data["node_path"] = node_path_str;
		result_data["property_name"] = property_name;
		result_data["old_value"] = old_value;
		result_data["new_value"] = value;
		result_data["warnings"] = ai_get_node_warnings(target_node);

		print_line(vformat("AI: Executed set_property. Node: %s, Property: %s, Value: %s", node_path_str, property_name, String(value)));
		return ai_create_success_result(result_data);
	}
}

Dictionary exec_rename_node(const Dictionary &args) {
	EditorUndoRedoManager *undo_redo = ai_get_undo_redo();
	if (!undo_redo) {
		return ai_create_error_result(AIErrorCodes::NO_UNDO_REDO,
			"EditorUndoRedoManager singleton not found");
	}

	Node *edited_scene_root = ai_get_edited_scene_root();
	if (!edited_scene_root) {
		return ai_create_error_result(AIErrorCodes::NO_ACTIVE_SCENE,
			"No edited scene root");
	}

	String node_path_str = args["node_path"];
	String new_name = args["new_name"];

	if (new_name.is_empty()) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"new_name cannot be empty");
	}

	Node *target = ai_get_node_by_path(node_path_str);
	if (!target) {
		return ai_node_not_found_error(node_path_str);
	}

	String old_name = target->get_name();

	ai_log_verbose(vformat("Renaming node '%s' from '%s' to '%s'", node_path_str, old_name, new_name));

	undo_redo->create_action("AI Rename Node");
	undo_redo->add_do_method(target, "set_name", new_name);
	undo_redo->add_undo_method(target, "set_name", old_name);
	undo_redo->commit_action();

	// Return success with details
	Dictionary result_data;
	result_data["node_path"] = node_path_str;
	result_data["old_name"] = old_name;
	result_data["new_name"] = new_name;

	print_line(vformat("AI: Executed rename_node. Path: %s, OldName: %s, NewName: %s", node_path_str, old_name, new_name));
	return ai_create_success_result(result_data);
}

Dictionary exec_reparent_node(const Dictionary &args) {
	EditorUndoRedoManager *undo_redo = ai_get_undo_redo();
	if (!undo_redo) {
		return ai_create_error_result(AIErrorCodes::NO_UNDO_REDO,
			"EditorUndoRedoManager singleton not found");
	}

	Node *edited_scene_root = ai_get_edited_scene_root();
	if (!edited_scene_root) {
		return ai_create_error_result(AIErrorCodes::NO_ACTIVE_SCENE,
			"No edited scene root");
	}

	String node_path_str = args["node_path"];
	String new_parent_path_str = args["new_parent_path"];

	// Resolve node
	Node *node = ai_get_node_by_path(node_path_str);
	if (!node) {
		return ai_node_not_found_error(node_path_str);
	}

	// Resolve new parent
	Node *new_parent = ai_get_node_by_path(new_parent_path_str);
	if (!new_parent) {
		return ai_node_not_found_error(new_parent_path_str);
	}

	// Reject if node is the scene root
	if (ai_is_scene_root(node)) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"Cannot reparent the scene root");
	}

	// Reject if node has no parent
	Node *old_parent = node->get_parent();
	if (!old_parent) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"Node has no parent");
	}

	// Save old index for undo
	int old_index = node->get_index();

	// Check if index argument is present
	bool has_index = args.has("index");
	int target_index = 0;
	if (has_index) {
		target_index = args["index"];
	}

	ai_log_verbose(vformat("Reparenting node '%s' from '%s' to '%s'", node_path_str, old_parent->get_path(), new_parent->get_path()));

	undo_redo->create_action("AI Reparent Node");

	// DO: remove from old parent, add to new parent
	undo_redo->add_do_method(old_parent, "remove_child", node);
	undo_redo->add_do_method(new_parent, "add_child", node, true); // force_readable_name = true
	undo_redo->add_do_method(node, "set_owner", edited_scene_root);

	// If index is specified, move to that position after adding
	if (has_index) {
		undo_redo->add_do_method(new_parent, "move_child", node, target_index);
	}

	// UNDO: remove from new parent, add back to old parent, restore old index
	undo_redo->add_undo_method(new_parent, "remove_child", node);
	undo_redo->add_undo_method(old_parent, "add_child", node, true);
	undo_redo->add_undo_method(node, "set_owner", edited_scene_root);
	undo_redo->add_undo_method(old_parent, "move_child", node, old_index);

	undo_redo->commit_action();

	// Return success with details
	Dictionary result_data;
	result_data["node_path"] = node_path_str;
	result_data["old_parent_path"] = old_parent->get_path();
	result_data["new_parent_path"] = new_parent->get_path();
	result_data["old_index"] = old_index;
	if (has_index) {
		result_data["new_index"] = target_index;
	}

	print_line(vformat("AI: Executed reparent_node. Node: %s, OldParent: %s, NewParent: %s", node_path_str, old_parent->get_name(), new_parent->get_name()));
	return ai_create_success_result(result_data);
}

Dictionary exec_delete_node(const Dictionary &args) {
	EditorUndoRedoManager *undo_redo = ai_get_undo_redo();
	if (!undo_redo) {
		return ai_create_error_result(AIErrorCodes::NO_UNDO_REDO,
			"EditorUndoRedoManager singleton not found");
	}

	Node *edited_scene_root = ai_get_edited_scene_root();
	if (!edited_scene_root) {
		return ai_create_error_result(AIErrorCodes::NO_ACTIVE_SCENE,
			"No edited scene root");
	}

	String node_path_str = args["node_path"];

	// Resolve node
	Node *node = ai_get_node_by_path(node_path_str);
	if (!node) {
		return ai_node_not_found_error(node_path_str);
	}

	// Reject if node is the scene root
	if (ai_is_scene_root(node)) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"Cannot delete the scene root");
	}

	// Reject if node has no parent
	Node *parent = node->get_parent();
	if (!parent) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"Node has no parent");
	}

	// Save old index for undo
	int old_index = node->get_index();
	String node_name = node->get_name();

	ai_log_verbose(vformat("Deleting node '%s' from parent '%s'", node_path_str, parent->get_path()));

	undo_redo->create_action("Delete Node");
	undo_redo->add_do_method(parent, "remove_child", node);
	undo_redo->add_do_reference(node); // Keep reference during undo/redo
	undo_redo->add_undo_method(parent, "add_child", node, true); // force_readable_name = true
	undo_redo->add_undo_method(node, "set_owner", edited_scene_root);
	undo_redo->add_undo_method(parent, "move_child", node, old_index);
	undo_redo->commit_action();

	// Return success with details
	Dictionary result_data;
	result_data["node_path"] = node_path_str;
	result_data["node_name"] = node_name;
	result_data["parent_path"] = parent->get_path();

	print_line(vformat("AI: Executed delete_node. Node: %s, Parent: %s", node_path_str, parent->get_path()));
	return ai_create_success_result(result_data);
}

Dictionary exec_duplicate_node(const Dictionary &args) {
	EditorUndoRedoManager *undo_redo = ai_get_undo_redo();
	if (!undo_redo) {
		return ai_create_error_result(AIErrorCodes::NO_UNDO_REDO,
			"EditorUndoRedoManager singleton not found");
	}

	Node *edited_scene_root = ai_get_edited_scene_root();
	if (!edited_scene_root) {
		return ai_create_error_result(AIErrorCodes::NO_ACTIVE_SCENE,
			"No edited scene root");
	}

	String node_path_str = args["node_path"];

	// Resolve node
	Node *node = ai_get_node_by_path(node_path_str);
	if (!node) {
		return ai_node_not_found_error(node_path_str);
	}

	// Reject if node is the scene root
	if (ai_is_scene_root(node)) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"Cannot duplicate the scene root");
	}

	// Reject if node has no parent
	Node *parent = node->get_parent();
	if (!parent) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"Node has no parent");
	}

	// Duplicate the node
	Node *dup = Object::cast_to<Node>(node->duplicate());
	if (!dup) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("Failed to duplicate node at path '%s'", node_path_str));
	}

	// Determine final name
	String final_name;
	if (args.has("new_name") && args["new_name"].get_type() == Variant::STRING) {
		String new_name = args["new_name"];
		if (!new_name.is_empty()) {
			final_name = new_name;
		} else {
			final_name = String(node->get_name()) + "_copy";
		}
	} else {
		final_name = String(node->get_name()) + "_copy";
	}

	ai_log_verbose(vformat("Duplicating node '%s' to '%s' under parent '%s'", node_path_str, final_name, parent->get_path()));

	undo_redo->create_action("Duplicate Node");
	undo_redo->add_do_method(parent, "add_child", dup, true); // force_readable_name = true
	undo_redo->add_do_method(dup, "set_name", final_name);
	undo_redo->add_do_method(dup, "set_owner", edited_scene_root);
	undo_redo->add_do_reference(dup); // Keep reference during undo/redo
	undo_redo->add_undo_method(parent, "remove_child", dup);
	undo_redo->add_undo_method(dup, "queue_free");
	undo_redo->commit_action();

	// Return success with details
	Dictionary result_data;
	result_data["original_node_path"] = node_path_str;
	result_data["duplicate_name"] = final_name;
	result_data["parent_path"] = parent->get_path();
	result_data["duplicate_path"] = dup->get_path();

	print_line(vformat("AI: Executed duplicate_node. Node: %s, Duplicate: %s, Parent: %s", node_path_str, final_name, parent->get_path()));
	return ai_create_success_result(result_data);
}

} // namespace AINodeActions
