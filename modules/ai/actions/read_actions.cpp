// modules/ai/actions/read_actions.cpp
// Read-only, introspection-related action implementations for the AI module

#include "read_actions.h"
#include "action_common.h"

#include "core/io/json.h"
#include "core/io/dir_access.h"
#include "core/io/image.h"
#include "core/io/resource.h"
#include "core/core_bind.h"
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
	// Root node uses "" so it doesn't collide with a child that shares the root's name.
	String path;
	if (root == relative_root) {
		path = "";
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
	entry["name"] = String(root->get_name());
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
	// Resolve which scene to inspect — defaults to currently edited scene if scene_path absent.
	String scene_path = args.get("scene_path", String());
	Node *scene_root = ai_resolve_scene_root_from_args(args);
	if (!scene_root) {
		if (!scene_path.is_empty()) {
			return ai_scene_not_open_error(scene_path);
		}
		return ai_create_error_result(AIErrorCodes::NO_ACTIVE_SCENE,
			"No edited scene root");
	}

	String root_path = args.get("root_path", String());

	Node *root_node = nullptr;
	if (root_path.is_empty()) {
		root_node = scene_root;
	} else {
		root_node = ai_get_node_by_path_in_root(scene_root, root_path);
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
	result_data["root_path"] = root_path.is_empty() ? String(scene_root->get_name()) : root_path;
	if (!scene_path.is_empty()) {
		result_data["scene_path"] = scene_path;
	}

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

	String scene_path = args.get("scene_path", String());
	Node *scene_root = ai_resolve_scene_root_from_args(args);
	if (!scene_root) {
		if (!scene_path.is_empty()) {
			return ai_scene_not_open_error(scene_path);
		}
		return ai_create_error_result(AIErrorCodes::NO_ACTIVE_SCENE,
			"No edited scene root");
	}

	String node_path = args["node_path"];

	Node *node = ai_get_node_by_path_in_root(scene_root, node_path);
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

	String scene_path = args.get("scene_path", String());
	Node *scene_root = ai_resolve_scene_root_from_args(args);
	if (!scene_root) {
		if (!scene_path.is_empty()) {
			return ai_scene_not_open_error(scene_path);
		}
		return ai_create_error_result(AIErrorCodes::NO_ACTIVE_SCENE,
			"No edited scene root");
	}

	String type_name_str = args["type_name"];
	StringName type_name = StringName(type_name_str);

	Array found_nodes;
	ai_find_nodes_by_type_dfs(scene_root, scene_root, type_name, found_nodes);

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
	if (!scene_path.is_empty()) {
		result_data["scene_path"] = scene_path;
	}

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
	// directory is optional (see tools_array.inc — it is not in the required list)
	// and defaults to the project root. Some models call list_files with only a
	// depth/glob and no directory; treat a missing or empty value as "res://"
	// rather than erroring. A present-but-non-string value is still a mistake.
	if (args.has("directory") && args["directory"].get_type() != Variant::STRING) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"'directory' must be a string");
	}

	String directory = "res://";
	if (args.has("directory") && !String(args["directory"]).is_empty()) {
		directory = args["directory"];
	}

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
		if (glob == "*" || glob == "*.*") {
			// Wildcard: no filter, include all files
		} else if (glob.begins_with("*.")) {
			suffix_filter = glob.substr(1); // Remove "*" prefix, keep ".ext"
		} else {
			return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
				vformat("Unsupported glob pattern '%s'. Use '*.gd', '*.*', or '*' to match all files", glob));
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
	bool dir_defaulted = !args.has("directory") || String(args["directory"]).is_empty();
	bool depth_defaulted = !args.has("depth");
	bool glob_defaulted = !args.has("glob");
	bool hidden_defaulted = !args.has("include_hidden");

	Dictionary display_args;
	display_args["directory"] = dir_defaulted ? Variant(vformat("%s (default)", directory)) : Variant(directory);
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

static const char *SUPPORTED_IMAGE_EXTENSIONS[] = {
	".png", ".jpg", ".jpeg", ".bmp", ".tga", ".webp", ".svg", nullptr
};

static bool _is_image_extension(const String &p_path) {
	String lower = p_path.to_lower();
	for (int i = 0; SUPPORTED_IMAGE_EXTENSIONS[i]; i++) {
		if (lower.ends_with(SUPPORTED_IMAGE_EXTENSIONS[i])) {
			return true;
		}
	}
	return false;
}

Dictionary exec_preview_asset(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	if (!args.has("paths") || args["paths"].get_type() != Variant::ARRAY) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"'paths' must be an array of image file paths");
	}

	Array paths = args["paths"];
	if (paths.is_empty()) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"'paths' array is empty");
	}

	// Auto-mode output budget: in "auto" scale, previews larger than this are
	// downscaled to fit. Explicit "Nx" divisors bypass it. Default 1024: roughly
	// 1,000-1,500 tokens per image, and most vision models cap useful resolution
	// around that size anyway.
	int max_size = 1024;
	if (args.has("max_size")) {
		Variant::Type mt = args["max_size"].get_type();
		if (mt == Variant::INT) {
			max_size = (int)args["max_size"];
		} else if (mt == Variant::FLOAT) {
			max_size = (int)(float)args["max_size"];
		}
		max_size = CLAMP(max_size, 32, 2048);
	}

	// Optional crop rect [x, y, w, h] in SOURCE pixels, applied to every path in the call.
	bool has_region = false;
	Rect2i region_req;
	if (args.has("region")) {
		if (args["region"].get_type() != Variant::ARRAY) {
			return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
				"'region' must be an array [x, y, w, h] in source pixels");
		}
		Array region_in = args["region"];
		if (region_in.size() != 4) {
			return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
				"'region' must have exactly 4 elements: [x, y, w, h]");
		}
		int region_vals[4];
		for (int i = 0; i < 4; i++) {
			Variant::Type rt = region_in[i].get_type();
			if (rt != Variant::INT && rt != Variant::FLOAT) {
				return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
					"'region' elements must be numbers: [x, y, w, h] in source pixels");
			}
			region_vals[i] = (int)region_in[i];
		}
		if (region_vals[2] <= 0 || region_vals[3] <= 0) {
			return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
				"'region' width and height must be positive");
		}
		region_req = Rect2i(region_vals[0], region_vals[1], region_vals[2], region_vals[3]);
		has_region = true;
	}

	// Scale knob: "auto" (default) returns native resolution when it fits the
	// budget, else downscales to fit. Explicit divisors — "1x" = source
	// resolution, "2x" = half, "4x" = quarter — are deliberate and uncapped.
	int scale_divisor = 0; // 0 = auto
	if (args.has("scale")) {
		String scale_str = String(args["scale"]).strip_edges().to_lower();
		if (!scale_str.is_empty() && scale_str != "auto") {
			bool scale_valid = scale_str.length() >= 2 && scale_str.ends_with("x");
			if (scale_valid) {
				String scale_num = scale_str.substr(0, scale_str.length() - 1);
				scale_valid = scale_num.is_valid_int() && scale_num.to_int() >= 1;
			}
			if (!scale_valid) {
				return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
					"'scale' must be \"auto\" or a divisor like \"1x\", \"2x\", \"4x\"");
			}
			scale_divisor = scale_str.substr(0, scale_str.length() - 1).to_int();
		}
	}

	Array previews;
	Array images;

	for (int i = 0; i < paths.size(); i++) {
		String path = paths[i];
		Dictionary entry;
		entry["path"] = path;

		if (!path.begins_with("res://")) {
			entry["error"] = "Path must start with 'res://'";
			previews.push_back(entry);
			continue;
		}

		if (!_is_image_extension(path)) {
			entry["error"] = "Not a supported image format";
			previews.push_back(entry);
			continue;
		}

		if (!FileAccess::exists(path)) {
			entry["error"] = "File not found";
			previews.push_back(entry);
			continue;
		}

		Ref<Image> img;
		img.instantiate();
		Error err = img->load(path);
		if (err != OK || img->is_empty()) {
			entry["error"] = "Failed to load image";
			previews.push_back(entry);
			continue;
		}

		// Keep the source dimensions: the thumbnail size alone misleads the model
		// into treating the scaled preview as the asset's real resolution.
		const int source_w = img->get_width();
		const int source_h = img->get_height();

		// Resolve the source rect this preview shows (whole image, or the crop).
		Rect2i shown(0, 0, source_w, source_h);
		if (has_region) {
			Rect2i clamped = region_req.intersection(shown);
			if (!clamped.has_area()) {
				entry["error"] = vformat("Region [%d, %d, %d, %d] lies outside the image (%dx%d)",
					region_req.position.x, region_req.position.y, region_req.size.x, region_req.size.y,
					source_w, source_h);
				previews.push_back(entry);
				continue;
			}
			if (clamped != region_req) {
				entry["region_clamped"] = true;
			}
			shown = clamped;
			img = img->get_region(shown);
		}

		// Pick the target size. Never upscale: output resolution is always at or
		// below source resolution. In auto mode, content that fits the budget at
		// native resolution passes through untouched; larger content is downscaled
		// to fit. Explicit "Nx" divisors bypass the budget — the model deliberately
		// accepted the context cost.
		const int shown_w = shown.size.x;
		const int shown_h = shown.size.y;
		const int longest = MAX(shown_w, shown_h);
		double scale = 1.0;
		if (scale_divisor > 0) {
			scale = 1.0 / (double)scale_divisor;
		} else if (longest > max_size) {
			scale = (double)max_size / (double)longest; // Fit to budget.
		}

		int target_w = MAX(1, (int)(shown_w * scale + 0.5));
		int target_h = MAX(1, (int)(shown_h * scale + 0.5));

		if (target_w != shown_w || target_h != shown_h) {
			img->resize(target_w, target_h, Image::INTERPOLATE_BILINEAR);
		}

		Vector<uint8_t> png_bytes = img->save_png_to_buffer();
		if (png_bytes.is_empty()) {
			entry["error"] = "Failed to encode PNG";
			previews.push_back(entry);
			continue;
		}

		PackedByteArray pba;
		pba.resize(png_bytes.size());
		memcpy(pba.ptrw(), png_bytes.ptr(), png_bytes.size());
		String b64 = CoreBind::Marshalls::get_singleton()->raw_to_base64(pba);

		entry["source_width"] = source_w;
		entry["source_height"] = source_h;
		// Echo the source rect actually shown so the model can map preview pixels
		// back to source pixels with zero arithmetic:
		//   source_x = region[0] + preview_x / effective_scale
		Array shown_region;
		shown_region.push_back(shown.position.x);
		shown_region.push_back(shown.position.y);
		shown_region.push_back(shown.size.x);
		shown_region.push_back(shown.size.y);
		entry["region"] = shown_region;
		entry["preview_width"] = img->get_width();
		entry["preview_height"] = img->get_height();
		// Preview px per source px (always <= 1.0; 1.0 = native 1:1, <1 downscaled).
		double effective_scale = (double)MAX(img->get_width(), img->get_height()) / (double)longest;
		effective_scale = (double)((int64_t)(effective_scale * 10000.0 + 0.5)) / 10000.0;
		entry["effective_scale"] = effective_scale;
		previews.push_back(entry);
		images.push_back(b64);
	}

	print_line(vformat("AI: preview_asset — %d paths, %d images generated (max_size=%d%s%s)",
		paths.size(), images.size(), max_size,
		has_region ? String(", region") : String(),
		scale_divisor > 0 ? vformat(", scale=%dx", scale_divisor) : String()));

	Dictionary result_data;
	result_data["previews"] = previews;
	result_data["image_count"] = images.size();
	if (!images.is_empty()) {
		result_data["_images"] = images;
	}

	return ai_create_success_result(result_data);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

} // namespace AIReadActions