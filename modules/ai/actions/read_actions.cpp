// modules/ai/actions/read_actions.cpp
// Read-only, introspection-related action implementations for the AI module

#include "read_actions.h"
#include "action_common.h"

#include "core/io/json.h"
#include "core/io/dir_access.h"
#include "scene/main/node.h"
#include "scene/2d/node_2d.h"
#include "scene/main/canvas_item.h"

namespace {

// Depth-first traversal to collect node information.
static void ai_collect_nodes_dfs(Node *root, Node *relative_root, Array &out_nodes) {
	if (!root || !relative_root) {
		return;
	}

	// Compute path relative to the chosen root.
	String path;
	if (root == relative_root) {
		path = relative_root->get_name();
	} else {
		NodePath rel_path = relative_root->get_path_to(root);
		path = String(rel_path);
	}

	// Determine node type and attached script path (if any).
	String type_name = root->get_class();
	String script_path;
	Ref<Script> script = root->get_script();
	if (script.is_valid()) {
		script_path = script->get_path();
	}

	Dictionary entry;
	entry["path"] = path;
	entry["type"] = type_name;
	if (!script_path.is_empty()) {
		entry["script"] = script_path;
	} else {
		entry["script"] = Variant(); // null
	}

	out_nodes.push_back(entry);

	// Recurse into children.
	const int child_count = root->get_child_count();
	for (int i = 0; i < child_count; i++) {
		Node *child = root->get_child(i);
		if (child) {
			ai_collect_nodes_dfs(child, relative_root, out_nodes);
		}
	}
}

// Depth-first traversal to find nodes matching a specific type.
static void ai_find_nodes_by_type_dfs(Node *root, Node *relative_root, const StringName &type_name, Array &out_nodes) {
	if (!root || !relative_root) {
		return;
	}

	// Check if this node matches the type.
	if (root->is_class(type_name)) {
		// Compute path relative to the chosen root.
		String path;
		if (root == relative_root) {
			path = String(relative_root->get_name());
		} else {
			NodePath rel_path = relative_root->get_path_to(root);
			path = String(rel_path);
		}

		Dictionary entry;
		entry["path"] = path;
		entry["name"] = root->get_name();
		out_nodes.push_back(entry);
	}

	// Recurse into children.
	const int child_count = root->get_child_count();
	for (int i = 0; i < child_count; i++) {
		Node *child = root->get_child(i);
		if (child) {
			ai_find_nodes_by_type_dfs(child, relative_root, type_name, out_nodes);
		}
	}
}

} // namespace

namespace AIReadActions {

bool exec_list_nodes(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	// Resolve root node.
	Node *edited_scene_root = ai_get_edited_scene_root();
	if (!edited_scene_root) {
		ai_log_error("Execute 'list_nodes': No edited scene root.");
		return false;
	}

	String root_path = args.get("root_path", String());

	Node *root_node = nullptr;
	if (root_path.is_empty()) {
		root_node = edited_scene_root;
	} else {
		root_node = ai_get_node_by_path(root_path);
	}

	if (!root_node) {
		ai_log_error(vformat("Execute 'list_nodes': Could not find root node at path '%s'.", root_path));
		return false;
	}

	Array nodes_info;
	ai_collect_nodes_dfs(root_node, root_node, nodes_info);

	String json = JSON::stringify(nodes_info);
	print_line("SMOKE/NODES: " + json);

	return true;
#else
	ai_log_error("Execute 'list_nodes': Editor API not available in non-editor builds.");
	return false;
#endif
}

bool exec_get_node_info(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	if (!args.has("node_path") || args["node_path"].get_type() != Variant::STRING) {
		ai_log_error("Execute 'get_node_info': 'node_path' must be a string.");
		return false;
	}

	String node_path = args["node_path"];

	Node *node = ai_get_node_by_path(node_path);
	if (!node) {
		ai_log_error(vformat("Execute 'get_node_info': Could not find node at path '%s'.", node_path));
		return false;
	}

	Dictionary info;
	info["path"] = node_path;
	info["type"] = node->get_class();
	info["name"] = node->get_name();

	String script_path;
	Ref<Script> script = node->get_script();
	if (script.is_valid()) {
		script_path = script->get_path();
	}
	if (!script_path.is_empty()) {
		info["script"] = script_path;
	} else {
		info["script"] = Variant(); // null
	}

	Dictionary props;

	// Node2D-specific properties.
	if (Node2D *n2d = Object::cast_to<Node2D>(node)) {
		props["position"] = n2d->get_position();
		props["rotation"] = n2d->get_rotation();
		props["scale"] = n2d->get_scale();
		props["global_position"] = n2d->get_global_position();
	}

	// CanvasItem-specific properties.
	if (CanvasItem *ci = Object::cast_to<CanvasItem>(node)) {
		props["visible"] = ci->is_visible();
	}

	info["properties"] = props;

	String json = JSON::stringify(info);
	print_line("READ/NODE_INFO: " + json);

	return true;
#else
	ai_log_error("Execute 'get_node_info': Editor API not available in non-editor builds.");
	return false;
#endif
}

bool exec_find_nodes_by_type(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	if (!args.has("type_name") || args["type_name"].get_type() != Variant::STRING) {
		ai_log_error("Execute 'find_nodes_by_type': 'type_name' must be a string.");
		return false;
	}

	Node *edited_scene_root = ai_get_edited_scene_root();
	if (!edited_scene_root) {
		ai_log_error("Execute 'find_nodes_by_type': No edited scene root.");
		return false;
	}

	String type_name_str = args["type_name"];
	StringName type_name = StringName(type_name_str);

	Array found_nodes;
	ai_find_nodes_by_type_dfs(edited_scene_root, edited_scene_root, type_name, found_nodes);

	int count = found_nodes.size();
	print_line(vformat("AI: Found %d node(s) of type '%s':", count, type_name_str));
	for (int i = 0; i < count; i++) {
		Dictionary entry = found_nodes[i];
		String path = entry["path"];
		String name = entry["name"];
		print_line(vformat("  - Path: %s, Name: %s", path, name));
	}

	return true;
#else
	ai_log_error("Execute 'find_nodes_by_type': Editor API not available in non-editor builds.");
	return false;
#endif
}

bool exec_list_files(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	if (!args.has("directory") || args["directory"].get_type() != Variant::STRING) {
		ai_log_error("Execute 'list_files': 'directory' must be a string.");
		return false;
	}

	String directory = args["directory"];

	// Validate directory starts with "res://"
	if (!directory.begins_with("res://")) {
		ai_log_error(vformat("Execute 'list_files': Directory must start with 'res://'. Got: %s", directory));
		return false;
	}

	// Open directory
	Ref<DirAccess> dir = DirAccess::open(directory);
	if (dir.is_null()) {
		ai_log_error(vformat("Execute 'list_files': Failed to open directory '%s'.", directory));
		return false;
	}

	// Get all files
	PackedStringArray files = dir->get_files();

	// Extract suffix from glob pattern if provided
	String suffix_filter;
	if (args.has("glob") && args["glob"].get_type() == Variant::STRING) {
		String glob = args["glob"];
		if (glob.begins_with("*.")) {
			suffix_filter = glob.substr(1); // Remove "*" prefix, keep ".ext"
		} else {
			ai_log_error(vformat("Execute 'list_files': Unsupported glob pattern '%s'. Only patterns like '*.gd' are supported.", glob));
			return false;
		}
	}

	// Filter files by suffix if glob provided
	PackedStringArray filtered_files;
	for (int i = 0; i < files.size(); i++) {
		String file = files[i];
		if (suffix_filter.is_empty() || file.ends_with(suffix_filter)) {
			filtered_files.push_back(file);
		}
	}

	// Log results
	int count = filtered_files.size();
	print_line(vformat("AI: Found %d file(s) in '%s':", count, directory));
	for (int i = 0; i < count; i++) {
		String file_path = directory;
		if (!file_path.ends_with("/")) {
			file_path += "/";
		}
		file_path += filtered_files[i];
		print_line(vformat("  - %s", file_path));
	}

	return true;
#else
	ai_log_error("Execute 'list_files': Editor API not available in non-editor builds.");
	return false;
#endif
}

} // namespace AIReadActions