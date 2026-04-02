// modules/ai/actions/node_actions.cpp
// Node-related action implementations for the AI module

#include "node_actions.h"
#include "action_common.h"

#include "core/object/class_db.h"
#include "core/string/node_path.h"
#include "core/io/json.h"
#include "core/io/resource.h"
#include "core/io/resource_loader.h"
#include "scene/main/node.h"

namespace {

// Coerce a JSON dict to a Godot math type given the expected Variant::Type.
static Variant ai_coerce_dict(const Dictionary &d, Variant::Type t) {
	switch (t) {
		case Variant::VECTOR2:  return Vector2((float)d.get("x", 0.0), (float)d.get("y", 0.0));
		case Variant::VECTOR3:  return Vector3((float)d.get("x", 0.0), (float)d.get("y", 0.0), (float)d.get("z", 0.0));
		case Variant::COLOR:    return Color((float)d.get("r", 0.0), (float)d.get("g", 0.0), (float)d.get("b", 0.0), (float)d.get("a", 1.0));
		case Variant::VECTOR2I: return Vector2i((int)d.get("x", 0), (int)d.get("y", 0));
		case Variant::VECTOR3I: return Vector3i((int)d.get("x", 0), (int)d.get("y", 0), (int)d.get("z", 0));
		default: return d;
	}
}

// Coerce a JSON array to a Godot math type given the expected Variant::Type.
static Variant ai_coerce_array(const Array &a, Variant::Type t) {
	switch (t) {
		case Variant::VECTOR2:  return Vector2(a.size() > 0 ? (float)a[0] : 0.0f, a.size() > 1 ? (float)a[1] : 0.0f);
		case Variant::VECTOR3:  return Vector3(a.size() > 0 ? (float)a[0] : 0.0f, a.size() > 1 ? (float)a[1] : 0.0f, a.size() > 2 ? (float)a[2] : 0.0f);
		case Variant::COLOR:    return Color(a.size() > 0 ? (float)a[0] : 0.0f, a.size() > 1 ? (float)a[1] : 0.0f, a.size() > 2 ? (float)a[2] : 0.0f, a.size() > 3 ? (float)a[3] : 1.0f);
		case Variant::VECTOR2I: return Vector2i(a.size() > 0 ? (int)a[0] : 0, a.size() > 1 ? (int)a[1] : 0);
		case Variant::VECTOR3I: return Vector3i(a.size() > 0 ? (int)a[0] : 0, a.size() > 1 ? (int)a[1] : 0, a.size() > 2 ? (int)a[2] : 0);
		default: return a;
	}
}

// Check whether an actual post-set value matches the intended target value.
// Handles floats (epsilon), resources (compare by path), and other types (==).
static bool ai_set_property_values_match(const Variant &actual, const Variant &target) {
	Variant::Type ta = actual.get_type();
	Variant::Type tt = target.get_type();

	// Both null/NIL
	if (ta == Variant::NIL && tt == Variant::NIL) {
		return true;
	}

	// Resource: compare by resource path when available, else by pointer identity
	if (ta == Variant::OBJECT) {
		Object *ao = actual.operator Object *();
		Object *to_obj = target.operator Object *();
		if (!ao && !to_obj) {
			return true;
		}
		if (!ao || !to_obj) {
			return false; // one is null
		}
		Resource *ar = Object::cast_to<Resource>(ao);
		Resource *tr = Object::cast_to<Resource>(to_obj);
		if (ar && tr) {
			String ap = ar->get_path();
			String tp = tr->get_path();
			if (!ap.is_empty() && !tp.is_empty()) {
				return ap == tp;
			}
		}
		return ao == to_obj; // pointer identity fallback
	}

	// Float: epsilon comparison
	if (ta == Variant::FLOAT && tt == Variant::FLOAT) {
		return Math::is_equal_approx((float)actual, (float)target);
	}

	// Color: component-wise with tolerance
	if (ta == Variant::COLOR && tt == Variant::COLOR) {
		Color ac = actual;
		Color tc = target;
		return Math::is_equal_approx(ac.r, tc.r) &&
			   Math::is_equal_approx(ac.g, tc.g) &&
			   Math::is_equal_approx(ac.b, tc.b) &&
			   Math::is_equal_approx(ac.a, tc.a);
	}

	// All other types: direct equality
	return actual == target;
}

// If value is a Dictionary or Array and the named property expects a math type, coerce it.
static Variant ai_coerce_value(Object *obj, const String &prop, const Variant &val) {
	// Unwrap {type, value} objects — the model sometimes wraps plain values (e.g. strings)
	// in a type-annotation dict like {"type": "String", "value": "hello"}.
	if (val.get_type() == Variant::DICTIONARY) {
		Dictionary d = val;
		if (d.size() == 2 && d.has("type") && d.has("value")) {
			return ai_coerce_value(obj, prop, d["value"]);
		}
	}

	// Single-element array wrapping a string — model sometimes sends ["res://..."] instead of "res://..."
	if (val.get_type() == Variant::ARRAY) {
		Array arr = val;
		if (arr.size() == 1 && arr[0].get_type() == Variant::STRING) {
			return ai_coerce_value(obj, prop, arr[0]);
		}
	}

	// String value on a resource-type property: load the resource.
	if (val.get_type() == Variant::STRING) {
		String path = val;
		if (path.begins_with("res://") || path.begins_with("user://")) {
			List<PropertyInfo> plist;
			obj->get_property_list(&plist);
			for (const PropertyInfo &pi : plist) {
				if (pi.name == prop && pi.hint == PROPERTY_HINT_RESOURCE_TYPE) {
					Ref<Resource> res = ResourceLoader::load(path);
					if (res.is_valid()) {
						return res;
					}
					break;
				}
			}
		}
		return val;
	}

	if (val.get_type() != Variant::DICTIONARY && val.get_type() != Variant::ARRAY) {
		return val;
	}
	List<PropertyInfo> plist;
	obj->get_property_list(&plist);
	for (const PropertyInfo &pi : plist) {
		if (pi.name == prop) {
			if (val.get_type() == Variant::ARRAY) {
				return ai_coerce_array(Array(val), pi.type);
			}
			return ai_coerce_dict(Dictionary(val), pi.type);
		}
	}
	return val;
}

} // namespace

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
		value = ai_coerce_value(target_node, property_name, value);
		Variant current_value = target_node->get(property_name);

		// Pre-validate: confirm the property exists before touching UndoRedo.
		bool prop_exists = false;
		target_node->get(StringName(property_name), &prop_exists);
		if (!prop_exists) {
			return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
				vformat("Property '%s' does not exist on %s. Check the property name.",
					property_name, target_node->get_class()));
		}

		undo_redo->create_action("AI Set Property");
		undo_redo->add_do_method(target_node, "set", property_name, value);
		undo_redo->add_undo_method(target_node, "set", property_name, current_value);
		undo_redo->commit_action();

		Variant actual_value = target_node->get(property_name);

		Dictionary result_data;
		result_data["node_path"] = node_path_str;
		result_data["property_name"] = property_name;
		result_data["old_value"] = current_value;
		result_data["target_value"] = value;
		result_data["actual_value"] = actual_value;
		result_data["warnings"] = ai_get_node_warnings(target_node);

		print_line(vformat("AI: Executed set_property. Node: %s, Property: %s, Value: %s", node_path_str, property_name, String(value)));

		if (!ai_set_property_values_match(actual_value, value)) {
			return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
				vformat("Property '%s' did not take the target value. Target: %s, Actual: %s. The property may be clamped, read-only, or rejected by the engine.",
					property_name, String(value), String(actual_value)));
		}
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
		value = ai_coerce_value(cur, final_prop, value);
		Variant old_value = cur->get(final_prop);

		// Pre-validate: confirm the final property exists on the traversed object.
		bool prop_exists = false;
		cur->get(StringName(final_prop), &prop_exists);
		if (!prop_exists) {
			return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
				vformat("Property '%s' does not exist on %s. Check the property name.",
					property_name, cur->get_class()));
		}

		undo_redo->create_action("AI Set Property");
		undo_redo->add_do_method(cur, "set", final_prop, value);
		undo_redo->add_undo_method(cur, "set", final_prop, old_value);
		undo_redo->commit_action();

		Variant actual_value = cur->get(final_prop);

		Dictionary result_data;
		result_data["node_path"] = node_path_str;
		result_data["property_name"] = property_name;
		result_data["old_value"] = old_value;
		result_data["target_value"] = value;
		result_data["actual_value"] = actual_value;
		result_data["warnings"] = ai_get_node_warnings(target_node);

		print_line(vformat("AI: Executed set_property. Node: %s, Property: %s, Value: %s", node_path_str, property_name, String(value)));

		if (!ai_set_property_values_match(actual_value, value)) {
			return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
				vformat("Property '%s' did not take the target value. Target: %s, Actual: %s. The property may be clamped, read-only, or rejected by the engine.",
					property_name, String(value), String(actual_value)));
		}
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

Dictionary exec_create_resource(const Dictionary &args) {
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

	if (!args.has("node_path") || args["node_path"].get_type() != Variant::STRING) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS, "'node_path' must be a string");
	}
	if (!args.has("property_name") || args["property_name"].get_type() != Variant::STRING) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS, "'property_name' must be a string");
	}
	if (!args.has("resource_type") || args["resource_type"].get_type() != Variant::STRING) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS, "'resource_type' must be a string");
	}

	String node_path_str = args["node_path"];
	String property_name = args["property_name"];
	String resource_type = args["resource_type"];

	Node *target_node = ai_get_node_by_path(node_path_str);
	if (!target_node) {
		return ai_node_not_found_error(node_path_str);
	}

	// Three-step ClassDB validation — no registry, no whitelist needed.
	StringName type_sname(resource_type);
	if (!ClassDB::class_exists(type_sname)) {
		return ai_create_error_result(AIErrorCodes::INVALID_TYPE,
			vformat("Resource type '%s' does not exist in ClassDB", resource_type));
	}
	if (!ClassDB::is_parent_class(type_sname, "Resource")) {
		return ai_create_error_result(AIErrorCodes::INVALID_TYPE,
			vformat("Type '%s' is not a Resource subclass", resource_type));
	}
	if (!ClassDB::can_instantiate(type_sname)) {
		return ai_create_error_result(AIErrorCodes::INVALID_TYPE,
			vformat("Type '%s' cannot be instantiated (abstract or virtual)", resource_type));
	}

	Object *obj = ClassDB::instantiate(type_sname);
	if (!obj) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("ClassDB::instantiate returned null for type '%s'", resource_type));
	}

	Resource *resource_raw = Object::cast_to<Resource>(obj);
	if (!resource_raw) {
		memdelete(obj);
		return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			vformat("Instantiated object for type '%s' is not a Resource", resource_type));
	}
	Ref<Resource> new_resource(resource_raw); // Takes ownership; no leak on any code path.

	// Apply optional initial properties before the UndoRedo assignment.
	// This way undo restores cleanly to old_value (no partial state).
	if (args.has("properties") && args["properties"].get_type() == Variant::DICTIONARY) {
		Dictionary props = args["properties"];
		Array keys = props.keys();
		for (int i = 0; i < keys.size(); i++) {
			String key = keys[i];
			Variant val = ai_coerce_value(resource_raw, key, props[key]);
			new_resource->set(key, val);
		}
	}

	// Resolve the target object and final property name, supporting dot notation.
	// e.g. "environment.sky.sky_material" → traverse to sky object, set "sky_material".
	Object *target_obj = target_node;
	String final_prop = property_name;

	if (property_name.contains(".")) {
		PackedStringArray segs = property_name.split(".");
		for (int i = 0; i < segs.size() - 1; i++) {
			Variant v = target_obj->get(segs[i]);
			if (v.get_type() != Variant::OBJECT) {
				return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
					vformat("Property '%s' is not a resource (got type %s). Assign a resource first.",
						segs[i], Variant::get_type_name(v.get_type())));
			}
			Object *next = v.operator Object *();
			if (!next) {
				return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
					vformat("Property '%s' is null. Use create_resource to assign a resource first.", segs[i]));
			}
			target_obj = next;
		}
		final_prop = segs[segs.size() - 1];
	}

	Variant old_value = target_obj->get(final_prop);

	// Pre-validate: confirm the property exists, then check PROPERTY_HINT_RESOURCE_TYPE.
	// Using get() with r_valid is side-effect-free (no setter calls, no GPU mutations).
	bool prop_exists = false;
	target_obj->get(StringName(final_prop), &prop_exists);
	if (!prop_exists) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			vformat("Property '%s' does not exist on %s. Check the property name.",
				property_name, target_obj->get_class()));
	}
	{
		// Walk property list once to validate the resource type hint.
		// hint_string may be comma-separated, e.g. "BaseMaterial3D,ShaderMaterial".
		List<PropertyInfo> plist;
		target_obj->get_property_list(&plist);
		for (const PropertyInfo &pi : plist) {
			if (pi.name != StringName(final_prop)) {
				continue;
			}
			if (pi.type == Variant::OBJECT && pi.hint == PROPERTY_HINT_RESOURCE_TYPE
					&& !pi.hint_string.is_empty()) {
				bool type_ok = false;
				Vector<String> accepted = pi.hint_string.split(",");
				for (const String &base_raw : accepted) {
					String base = base_raw.strip_edges();
					if (!base.is_empty() && ClassDB::is_parent_class(type_sname, StringName(base))) {
						type_ok = true;
						break;
					}
				}
				if (!type_ok) {
					return ai_create_error_result(AIErrorCodes::INVALID_TYPE,
						vformat("Resource type '%s' is not compatible with property '%s' (expected: %s).",
							resource_type, property_name, pi.hint_string));
				}
			}
			break;
		}
	}

	undo_redo->create_action("AI Create Resource");
	undo_redo->add_do_method(target_obj, "set", final_prop, new_resource);
	undo_redo->add_undo_method(target_obj, "set", final_prop, old_value);
	undo_redo->commit_action();

	Dictionary result_data;
	result_data["node_path"] = node_path_str;
	result_data["property_name"] = property_name;
	result_data["resource_type"] = resource_type;
	result_data["old_value"] = old_value;
	result_data["warnings"] = ai_get_node_warnings(target_node);

	print_line(vformat("AI: Executed create_resource. Node: %s, Property: %s, Type: %s",
		node_path_str, property_name, resource_type));
	return ai_create_success_result(result_data);
}

} // namespace AINodeActions
