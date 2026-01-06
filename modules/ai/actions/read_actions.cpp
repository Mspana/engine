// modules/ai/actions/read_actions.cpp
// Read-only, introspection-related action implementations for the AI module

#include "read_actions.h"
#include "action_common.h"

#include "core/io/json.h"
#include "scene/main/node.h"

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

} // namespace AIReadActions