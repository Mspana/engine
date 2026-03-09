// modules/ai/actions/read_actions.cpp
// Read-only, introspection-related action implementations for the AI module

#include "read_actions.h"
#include "action_common.h"

#include "core/io/json.h"
#include "core/io/dir_access.h"
#include "core/io/resource.h"
#include "scene/main/node.h"
#include "modules/gdscript/gdscript_parser.h"
#include "modules/gdscript/gdscript_analyzer.h"

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
	entry["has_warnings"] = !root->get_configuration_warnings().is_empty();

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

// Recursively scans a Resource's properties into a Dictionary.
// depth: levels of sub-resource nesting to expand.
//   1 = this resource's primitives only (no nested resources in sub_resources)
//   2+ = recurse that many levels
//   -1 = unlimited recursion
//   0 = return type only, no properties or sub_resources
static Dictionary ai_scan_resource(Resource *res, int depth) {
	Dictionary result;
	result["type"] = res->get_class();
	if (depth == 0) {
		return result;
	}
	Dictionary props;
	Dictionary sub;

	List<PropertyInfo> rpl;
	res->get_property_list(&rpl);
	for (const PropertyInfo &rpi : rpl) {
		if (rpi.name.is_empty()) continue;
		if (rpi.usage & PROPERTY_USAGE_INTERNAL) continue;
		if (!(rpi.usage & PROPERTY_USAGE_STORAGE)) continue;
		Variant rval = res->get(rpi.name);
		if (rpi.hint == PROPERTY_HINT_RESOURCE_TYPE) {
			Object *obj = (rval.get_type() == Variant::OBJECT) ? rval.operator Object *() : nullptr;
			Resource *nested = Object::cast_to<Resource>(obj);
			sub[rpi.name] = nested ? Variant(ai_scan_resource(nested, depth > 0 ? depth - 1 : depth)) : Variant();
		} else if (rval.get_type() != Variant::OBJECT) {
			props[rpi.name] = rval;
		}
	}
	result["properties"] = props;
	result["sub_resources"] = sub;
	return result;
}

} // namespace

namespace AIReadActions {

Dictionary exec_list_nodes(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	// Resolve root node.
	Node *edited_scene_root = ai_get_edited_scene_root();
	if (!edited_scene_root) {
		return ai_create_error_result(AIErrorCodes::NO_ACTIVE_SCENE,
			"No edited scene root");
	}

	String root_path = args.get("root_path", String());

	Node *root_node = nullptr;
	if (root_path.is_empty()) {
		root_node = edited_scene_root;
	} else {
		root_node = ai_get_node_by_path(root_path);
	}

	if (!root_node) {
		return ai_create_error_result(AIErrorCodes::NODE_NOT_FOUND,
			vformat("Could not find root node at path '%s'", root_path));
	}

	Array nodes_info;
	ai_collect_nodes_dfs(root_node, root_node, nodes_info);

	String json = JSON::stringify(nodes_info);
	print_line("SMOKE/NODES: " + json);

	Dictionary result_data;
	result_data["nodes"] = nodes_info;
	result_data["count"] = nodes_info.size();
	result_data["root_path"] = root_path.is_empty() ? String(edited_scene_root->get_name()) : root_path;

	return ai_create_success_result(result_data);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

Dictionary exec_get_node_info(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	if (!args.has("node_path") || args["node_path"].get_type() != Variant::STRING) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"'node_path' must be a string");
	}

	String node_path = args["node_path"];

	Node *node = ai_get_node_by_path(node_path);
	if (!node) {
		return ai_node_not_found_error(node_path);
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

	// resource_depth: how many levels of sub-resource nesting to expand.
	// 1 = resource type + its primitives (default); 2+ = recurse deeper; -1 = unlimited; 0 = type only.
	int resource_depth = (int)args.get("resource_depth", 1);

	Dictionary props;
	Dictionary sub_resources;

	List<PropertyInfo> prop_list;
	node->get_property_list(&prop_list);
	for (const PropertyInfo &pi : prop_list) {
		if (pi.name.is_empty()) continue;
		if (pi.usage & PROPERTY_USAGE_INTERNAL) continue;
		if (!(pi.usage & PROPERTY_USAGE_STORAGE)) continue;

		Variant val = node->get(pi.name);

		if (pi.hint == PROPERTY_HINT_RESOURCE_TYPE) {
			Object *obj = (val.get_type() == Variant::OBJECT) ? val.operator Object *() : nullptr;
			Resource *res = Object::cast_to<Resource>(obj);
			if (!res) {
				sub_resources[pi.name] = Variant(); // null = not assigned
			} else {
				sub_resources[pi.name] = ai_scan_resource(res, resource_depth);
			}
		} else if (val.get_type() != Variant::OBJECT) {
			props[pi.name] = val;
		}
	}

	info["properties"] = props;
	info["sub_resources"] = sub_resources;
	info["warnings"] = ai_get_node_warnings(node);

	String json = JSON::stringify(info);
	print_line("READ/NODE_INFO: " + json);

	return ai_create_success_result(info);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

Dictionary exec_find_nodes_by_type(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	if (!args.has("type_name") || args["type_name"].get_type() != Variant::STRING) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"'type_name' must be a string");
	}

	Node *edited_scene_root = ai_get_edited_scene_root();
	if (!edited_scene_root) {
		return ai_create_error_result(AIErrorCodes::NO_ACTIVE_SCENE,
			"No edited scene root");
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

	Dictionary result_data;
	result_data["nodes"] = found_nodes;
	result_data["count"] = count;
	result_data["type_name"] = type_name_str;

	return ai_create_success_result(result_data);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

static void _collect_tree(const String &p_dir, int p_current_depth, int p_max_depth,
		const String &p_suffix_filter, bool p_include_hidden, Array &r_entries) {
	Ref<DirAccess> dir = DirAccess::open(p_dir);
	if (dir.is_null()) {
		return;
	}
	String base = p_dir.trim_suffix("/") + "/";

	// Directories first (no glob filter on dirs); trailing "/" signals directory
	PackedStringArray subdirs = dir->get_directories();
	for (int i = 0; i < subdirs.size(); i++) {
		// On Windows, DirAccess::current_is_hidden() checks FILE_ATTRIBUTE_HIDDEN, not dot-prefix.
		// Manually skip dot-prefixed dirs when include_hidden is false.
		if (!p_include_hidden && subdirs[i].begins_with(".")) {
			continue;
		}
		String d = base + subdirs[i] + "/";
		r_entries.push_back(d);
		if (p_max_depth == 0 || p_current_depth < p_max_depth) {
			_collect_tree(d, p_current_depth + 1, p_max_depth, p_suffix_filter, p_include_hidden, r_entries);
		}
	}

	// Files, filtered by suffix if glob provided
	PackedStringArray files = dir->get_files();
	for (int i = 0; i < files.size(); i++) {
		if (!p_include_hidden && files[i].begins_with(".")) {
			continue;
		}
		if (!p_suffix_filter.is_empty() && !files[i].ends_with(p_suffix_filter)) {
			continue;
		}
		r_entries.push_back(base + files[i]);
	}
}

Dictionary exec_list_files(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	if (!args.has("directory") || args["directory"].get_type() != Variant::STRING) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"'directory' must be a string");
	}

	String directory = args["directory"];

	// Validate directory starts with "res://"
	if (!directory.begins_with("res://")) {
		return ai_create_error_result(AIErrorCodes::INVALID_PATH,
			vformat("Directory must start with 'res://'. Got: %s", directory));
	}

	// Open directory
	Ref<DirAccess> dir = DirAccess::open(directory);
	if (dir.is_null()) {
		return ai_create_error_result(AIErrorCodes::FILE_NOT_FOUND,
			vformat("Failed to open directory '%s'", directory));
	}

	// Extract suffix from glob pattern if provided (applied to files only)
	String suffix_filter;
	if (args.has("glob") && args["glob"].get_type() == Variant::STRING) {
		String glob = args["glob"];
		if (glob.begins_with("*.")) {
			suffix_filter = glob.substr(1); // Remove "*" prefix, keep ".ext"
		} else {
			return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
				vformat("Unsupported glob pattern '%s'. Only patterns like '*.gd' are supported", glob));
		}
	}

	// depth: 1 = top level only (default), 0 = unlimited, N = N levels deep
	// Accept float too — JSON round numbers (0.0, 1.0) arrive as FLOAT from some providers
	int max_depth = 1;
	if (args.has("depth")) {
		Variant::Type dt = args["depth"].get_type();
		if (dt == Variant::INT) {
			max_depth = (int)args["depth"];
		} else if (dt == Variant::FLOAT) {
			max_depth = (int)(float)args["depth"];
		}
		if (max_depth < 0) {
			max_depth = 0;
		}
	}

	bool include_hidden = false;
	if (args.has("include_hidden") && args["include_hidden"].get_type() == Variant::BOOL) {
		include_hidden = (bool)args["include_hidden"];
	}

	Array objects;
	_collect_tree(directory, 1, max_depth, suffix_filter, include_hidden, objects);
	objects.sort();

	print_line(vformat("AI: list_files '%s' depth=%d → %d objects", directory, max_depth, objects.size()));

	// Build resolved args for display (fills in defaults, normalises float depth → int)
	// Defaulted params are shown as strings with "(default)" annotation.
	bool depth_defaulted = !args.has("depth");
	bool glob_defaulted = !args.has("glob");
	bool hidden_defaulted = !args.has("include_hidden");

	Dictionary display_args;
	display_args["directory"] = directory;
	display_args["depth"] = depth_defaulted ? Variant(vformat("%d (default)", max_depth)) : Variant(max_depth);
	display_args["glob"] = glob_defaulted ? Variant(String("null (default)")) : (suffix_filter.is_empty() ? Variant() : Variant(suffix_filter));
	display_args["include_hidden"] = hidden_defaulted ? Variant(vformat("%s (default)", include_hidden ? "true" : "false")) : Variant(include_hidden);

	Dictionary result_data;
	result_data["objects"] = objects;
	result_data["_display_args"] = display_args;

	return ai_create_success_result(result_data);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

Dictionary exec_read_script(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	if (!args.has("file_path") || args["file_path"].get_type() != Variant::STRING) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"'file_path' must be a string");
	}

	String file_path = args["file_path"];

	if (!file_path.begins_with("res://")) {
		return ai_create_error_result(AIErrorCodes::INVALID_PATH,
			vformat("File path must start with 'res://'. Got: %s", file_path));
	}

	if (!FileAccess::exists(file_path)) {
		return ai_create_error_result(AIErrorCodes::FILE_NOT_FOUND,
			vformat("File does not exist: %s", file_path));
	}

	Ref<FileAccess> file = FileAccess::open(file_path, FileAccess::READ);
	if (file.is_null()) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("Failed to open file: %s", file_path));
	}

	String content = file->get_as_text();

	// Validate GDScript syntax + semantics (same pipeline as update_script)
	Array parse_errors;
	Array warnings;
	if (file_path.ends_with(".gd")) {
		GDScriptParser parser;
		Error parse_err = parser.parse(content, file_path, false);

		for (const GDScriptParser::ParserError &e : parser.get_errors()) {
			Dictionary err_dict;
			err_dict["line"] = e.line;
			err_dict["column"] = e.column;
			err_dict["message"] = e.message;
			err_dict["type"] = "syntax";
			parse_errors.push_back(err_dict);
		}

		if (parse_err == OK && parser.get_errors().is_empty()) {
			GDScriptAnalyzer analyzer(&parser);
			analyzer.analyze();
			for (const GDScriptParser::ParserError &e : parser.get_errors()) {
				Dictionary err_dict;
				err_dict["line"] = e.line;
				err_dict["column"] = e.column;
				err_dict["message"] = e.message;
				err_dict["type"] = "semantic";
				parse_errors.push_back(err_dict);
			}
		}

#ifdef DEBUG_ENABLED
		for (const GDScriptWarning &w : parser.get_warnings()) {
			Dictionary warn_dict;
			warn_dict["line"] = w.start_line;
			warn_dict["message"] = w.get_message();
			warn_dict["code"] = w.get_name();
			warnings.push_back(warn_dict);
		}
#endif
	}

	Dictionary result_data;
	result_data["file_path"] = file_path;
	result_data["content"] = content;
	result_data["size"] = content.length();
	if (!parse_errors.is_empty()) {
		result_data["parse_errors"] = parse_errors;
	}
	if (!warnings.is_empty()) {
		result_data["warnings"] = warnings;
	}

	print_line(vformat("AI: Read script '%s' (%d chars)", file_path, content.length()));
	return ai_create_success_result(result_data);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

} // namespace AIReadActions