// tools_array.inc — Native tool definitions for Aristotle AI module
// Included by ai_provider.cpp inside AIProvider::build_tools_array()
// Each block pushes one tool definition into the `tools` Array.
// Uses _make_prop() and _make_tool() helpers defined in ai_provider.cpp.

// ==========================================================================
// Node actions
// ==========================================================================
{
	Dictionary props;
	props["node_name"] = _make_prop("string", "Name for the new node");
	props["node_type"] = _make_prop("string", "Godot node type (e.g. CharacterBody2D, Sprite2D, Camera2D)");
	props["parent_path"] = _make_prop("string", "Path relative to scene root. Omit for root child.");
	Array req;
	req.push_back("node_name");
	req.push_back("node_type");
	tools.push_back(_make_tool("create_node", "Create a new node in the scene tree", props, req));
}
{
	Dictionary props;
	props["node_path"] = _make_prop("string", "Path of the node to delete (relative to scene root)");
	Array req;
	req.push_back("node_path");
	tools.push_back(_make_tool("delete_node", "Delete a node from the scene tree", props, req));
}
{
	Dictionary props;
	props["node_path"] = _make_prop("string", "Path of the node to duplicate");
	props["new_name"] = _make_prop("string", "Name for the duplicate (optional, defaults to <name>_copy)");
	Array req;
	req.push_back("node_path");
	tools.push_back(_make_tool("duplicate_node", "Duplicate a node in the scene tree", props, req));
}
{
	Dictionary props;
	props["node_path"] = _make_prop("string", "Path of the node");
	props["property_name"] = _make_prop("string", "Property name. Use dot notation for sub-resource properties (e.g. 'mesh.size', 'material.albedo_color')");
	// value can be any type, but JSON schema requires a type — use object-level description
	Dictionary value_prop;
	value_prop["description"] = "The value to set. For Vector2/Vector3/Color use arrays: [x,y] or [x,y,z].";
	props["value"] = value_prop;
	Array req;
	req.push_back("node_path");
	req.push_back("property_name");
	req.push_back("value");
	tools.push_back(_make_tool("set_property", "Set a property on a node or its sub-resource", props, req));
}
{
	Dictionary props;
	props["node_path"] = _make_prop("string", "Path of the node to assign the resource to");
	props["property_name"] = _make_prop("string", "Property name (supports dot notation for nested slots)");
	props["resource_type"] = _make_prop("string", "Concrete resource class (e.g. BoxMesh, SphereShape3D, not abstract Mesh/Shape3D)");
	Dictionary props_prop;
	props_prop["type"] = "object";
	props_prop["description"] = "Optional initial property values for the resource";
	props["properties"] = props_prop;
	Array req;
	req.push_back("node_path");
	req.push_back("property_name");
	req.push_back("resource_type");
	tools.push_back(_make_tool("create_resource", "Create a new Resource and assign it to a node property", props, req));
}
{
	Dictionary props;
	props["node_path"] = _make_prop("string", "Path of the node to rename");
	props["new_name"] = _make_prop("string", "New name for the node");
	Array req;
	req.push_back("node_path");
	req.push_back("new_name");
	tools.push_back(_make_tool("rename_node", "Rename a node", props, req));
}
{
	Dictionary props;
	props["node_path"] = _make_prop("string", "Path of the node to move");
	props["new_parent_path"] = _make_prop("string", "Path of the new parent node");
	props["index"] = _make_prop("integer", "Position among siblings (optional)");
	Array req;
	req.push_back("node_path");
	req.push_back("new_parent_path");
	tools.push_back(_make_tool("reparent_node", "Move a node to a new parent", props, req));
}

// ==========================================================================
// Script actions
// ==========================================================================
{
	Dictionary props;
	props["file_path"] = _make_prop("string", "Script path (e.g. res://scripts/Enemy.gd)");
	props["language"] = _make_prop("string", "Script language (currently only 'GDScript')");
	props["content"] = _make_prop("string", "Full script content");
	Array req;
	req.push_back("file_path");
	req.push_back("language");
	req.push_back("content");
	tools.push_back(_make_tool("create_script", "Create a new GDScript file. Validates with compiler; check parse_errors in result.", props, req));
}
{
	Dictionary props;
	props["file_path"] = _make_prop("string", "Path of the script to update (e.g. res://scripts/player.gd)");
	props["old_string"] = _make_prop("string", "The exact text to find and replace");
	props["new_string"] = _make_prop("string", "The replacement text (must differ from old_string)");
	props["replace_all"] = _make_prop("boolean", "Replace all occurrences (default false)");
	Array req;
	req.push_back("file_path");
	req.push_back("old_string");
	req.push_back("new_string");
	tools.push_back(_make_tool("update_script", "Performs exact string replacement in a script file. You MUST have called read_script or create_script on this file first — this tool will fail otherwise. The edit will FAIL if old_string is not found or is not unique in the file (provide more surrounding context to disambiguate, or set replace_all to true). Validates with compiler; check parse_errors in result.", props, req));
}
{
	Dictionary props;
	props["node_path"] = _make_prop("string", "Path of the node to attach the script to");
	props["script_path"] = _make_prop("string", "Path of the script file (e.g. res://scripts/player.gd)");
	Array req;
	req.push_back("node_path");
	req.push_back("script_path");
	tools.push_back(_make_tool("attach_script", "Attach a script to a node", props, req));
}
{
	Dictionary props;
	props["node_path"] = _make_prop("string", "Path of the node to detach the script from");
	Array req;
	req.push_back("node_path");
	tools.push_back(_make_tool("detach_script", "Detach a script from a node", props, req));
}
{
	Dictionary props;
	props["old_path"] = _make_prop("string", "Current script file path");
	props["new_path"] = _make_prop("string", "New script file path");
	Array req;
	req.push_back("old_path");
	req.push_back("new_path");
	tools.push_back(_make_tool("rename_script", "Rename/move a script file", props, req));
}
{
	Dictionary props;
	props["file_path"] = _make_prop("string", "Path of the script to delete");
	props["detach_from_nodes"] = _make_prop("boolean", "If true, detach from all nodes before deleting (default false)");
	Array req;
	req.push_back("file_path");
	tools.push_back(_make_tool("delete_script", "Delete a script file", props, req));
}

// ==========================================================================
// Signal actions
// ==========================================================================
{
	Dictionary props;
	props["emitter_path"] = _make_prop("string", "Path of the node emitting the signal");
	props["signal_name"] = _make_prop("string", "Signal name (e.g. 'pressed', 'body_entered')");
	props["target_path"] = _make_prop("string", "Path of the node with the target method");
	props["method_name"] = _make_prop("string", "Method name to connect (e.g. '_on_button_pressed')");
	Array req;
	req.push_back("emitter_path");
	req.push_back("signal_name");
	req.push_back("target_path");
	req.push_back("method_name");
	tools.push_back(_make_tool("connect_signal", "Connect a signal from an emitter node to a target method", props, req));
}
{
	Dictionary props;
	props["emitter_path"] = _make_prop("string", "Path of the emitter node");
	props["signal_name"] = _make_prop("string", "Signal name");
	props["target_path"] = _make_prop("string", "Path of the target node");
	props["method_name"] = _make_prop("string", "Method name to disconnect");
	Array req;
	req.push_back("emitter_path");
	req.push_back("signal_name");
	req.push_back("target_path");
	req.push_back("method_name");
	tools.push_back(_make_tool("disconnect_signal", "Disconnect a signal connection", props, req));
}

// ==========================================================================
// Scene actions
// ==========================================================================
{
	Dictionary props;
	props["scene_path"] = _make_prop("string", "Scene file path (e.g. res://scenes/Main.tscn)");
	props["root_type"] = _make_prop("string", "Root node type (default: Node)");
	props["root_name"] = _make_prop("string", "Root node name (default: derived from filename)");
	Array req;
	req.push_back("scene_path");
	tools.push_back(_make_tool("create_scene", "Create a new scene file", props, req));
}
{
	Dictionary props;
	props["scene_path"] = _make_prop("string", "Scene file path to open");
	Array req;
	req.push_back("scene_path");
	tools.push_back(_make_tool("open_scene", "Open a scene file in the editor", props, req));
}
{
	Dictionary props;
	Array req;
	tools.push_back(_make_tool("save_scene", "Save the currently edited scene", props, req));
}
{
	Dictionary props;
	props["save_if_modified"] = _make_prop("boolean", "Save before closing if modified (default true)");
	Array req;
	tools.push_back(_make_tool("close_scene", "Close the current scene tab", props, req));
}
{
	Dictionary props;
	props["scene_path"] = _make_prop("string", "Scene file path to set as main scene");
	Array req;
	req.push_back("scene_path");
	tools.push_back(_make_tool("set_main_scene", "Set the project's main scene", props, req));
}
{
	Dictionary props;
	Array req;
	tools.push_back(_make_tool("list_open_scenes", "List all currently open scene tabs in the editor", props, req));
}

// ==========================================================================
// Project actions
// ==========================================================================
{
	Dictionary props;
	props["key"] = _make_prop("string", "Setting key (e.g. display/window/size/viewport_width)");
	Dictionary value_prop;
	value_prop["description"] = "Setting value";
	props["value"] = value_prop;
	Array req;
	req.push_back("key");
	req.push_back("value");
	tools.push_back(_make_tool("set_project_setting", "Set a project setting value", props, req));
}
{
	Dictionary props;
	props["prefix"] = _make_prop("string", "Filter by prefix (e.g. 'display/')");
	Dictionary keys_prop;
	keys_prop["type"] = "array";
	keys_prop["description"] = "Specific keys to retrieve";
	Dictionary keys_items;
	keys_items["type"] = "string";
	keys_prop["items"] = keys_items;
	props["keys"] = keys_prop;
	props["include_defaults"] = _make_prop("boolean", "Include default values (default false)");
	Array req;
	tools.push_back(_make_tool("get_project_settings", "Get project settings (read-only)", props, req));
}
{
	Dictionary props;
	props["name"] = _make_prop("string", "Autoload singleton name");
	props["script_path"] = _make_prop("string", "Script path (e.g. res://scripts/GameManager.gd)");
	props["enabled"] = _make_prop("boolean", "Enable the autoload (default true)");
	Array req;
	req.push_back("name");
	req.push_back("script_path");
	tools.push_back(_make_tool("create_autoload_singleton", "Create an autoload singleton", props, req));
}
{
	Dictionary props;
	props["name"] = _make_prop("string", "Autoload singleton name to remove");
	Array req;
	req.push_back("name");
	tools.push_back(_make_tool("remove_autoload_singleton", "Remove an autoload singleton", props, req));
}
{
	Dictionary props;
	props["source_path"] = _make_prop("string", "Absolute OS path of the source file");
	props["dest_path"] = _make_prop("string", "Destination path (res://...)");
	props["overwrite"] = _make_prop("boolean", "Overwrite if exists (default false)");
	Array req;
	req.push_back("source_path");
	req.push_back("dest_path");
	tools.push_back(_make_tool("import_asset", "Import an asset file into the project", props, req));
}
{
	Dictionary props;
	props["asset_path"] = _make_prop("string", "Asset path to delete (res://...)");
	Array req;
	req.push_back("asset_path");
	tools.push_back(_make_tool("delete_asset", "Delete an asset file from the project", props, req));
}

// ==========================================================================
// Run / test actions
// ==========================================================================
{
	Dictionary props;
	props["mode"] = _make_prop("string", "Run mode: 'play' (default) or 'headless_smoke'");
	props["scene_path"] = _make_prop("string", "Scene to run (optional, uses main scene if omitted)");
	Array req;
	tools.push_back(_make_tool("run_project", "Run/play the project", props, req));
}
{
	Dictionary props;
	props["wait_seconds"] = _make_prop("number", "Seconds to let game run before capturing (default 2.0, range 0.5-10.0)");
	Array req;
	tools.push_back(_make_tool("run_and_screenshot", "Run the game briefly, capture a screenshot, then stop. Use to visually verify changes.", props, req));
}
{
	Dictionary props;
	Array req;
	tools.push_back(_make_tool("stop_game", "Stop the currently running game. No-op if the game is not running. Returns was_playing bool.", props, req));
}

// ==========================================================================
// Read / inspect actions
// ==========================================================================
{
	Dictionary props;
	props["root_path"] = _make_prop("string", "Root node path to list from (optional)");
	Array req;
	tools.push_back(_make_tool("list_nodes", "List nodes in the current scene tree", props, req));
}
{
	Dictionary props;
	props["node_path"] = _make_prop("string", "Path of the node to inspect");
	props["resource_depth"] = _make_prop("integer", "Resource inspection depth (0=type only, 1=primitives, 2+=recurse, -1=unlimited; default 1)");
	Array req;
	req.push_back("node_path");
	tools.push_back(_make_tool("get_node_info", "Get detailed information about a single node (properties, sub_resources, warnings)", props, req));
}
{
	Dictionary props;
	props["type_name"] = _make_prop("string", "Node type to search for (e.g. CharacterBody2D)");
	Array req;
	req.push_back("type_name");
	tools.push_back(_make_tool("find_nodes_by_type", "Find all nodes of a specific type in the scene tree", props, req));
}
{
	Dictionary props;
	props["directory"] = _make_prop("string", "Directory to list (e.g. res://scripts)");
	props["depth"] = _make_prop("integer", "Recursion depth (default 1 = top level only, 0 = full recursion)");
	props["glob"] = _make_prop("string", "File filter pattern (e.g. '*.gd')");
	props["include_hidden"] = _make_prop("boolean", "Include dot-prefixed files/dirs (default false)");
	Array req;
	tools.push_back(_make_tool("list_files", "List files and directories (like 'tree')", props, req));
}
{
	Dictionary props;
	props["file_path"] = _make_prop("string", "Script file path to read (e.g. res://scripts/player.gd)");
	Array req;
	req.push_back("file_path");
	tools.push_back(_make_tool("read_script", "Read the source content of a script file. For .gd files, also validates with the GDScript compiler.", props, req));
}

// ==========================================================================
// Internal / meta actions
// ==========================================================================
{
	Dictionary props;
	props["summary"] = _make_prop("string", "Brief summary of the insight");
	Dictionary arr_prop;
	arr_prop["type"] = "array";
	Dictionary arr_items;
	arr_items["type"] = "string";
	arr_prop["items"] = arr_items;
	props["friction_points"] = arr_prop;
	props["missing_tools"] = arr_prop;
	props["schema_suggestions"] = arr_prop;
	props["prompt_suggestions"] = arr_prop;
	props["bugs_suspected"] = arr_prop;
	props["next_debug_steps"] = arr_prop;
	props["freeform"] = _make_prop("string", "Free-form notes");
	Array req;
	req.push_back("summary");
	tools.push_back(_make_tool("write_dev_note", "Record a developer insight to the AI journal", props, req));
}
{
	Dictionary props;
	Dictionary todos_prop;
	todos_prop["type"] = "array";
	todos_prop["description"] = "Full replacement task list";
	Dictionary todo_items;
	todo_items["type"] = "object";
	Dictionary todo_props;
	todo_props["id"] = _make_prop("string", "Task ID");
	todo_props["content"] = _make_prop("string", "Task description (5-10 words)");
	todo_props["status"] = _make_prop("string", "pending, in_progress, or completed");
	todo_items["properties"] = todo_props;
	Array todo_req;
	todo_req.push_back("id");
	todo_req.push_back("content");
	todo_req.push_back("status");
	todo_items["required"] = todo_req;
	todos_prop["items"] = todo_items;
	props["todos"] = todos_prop;
	Array req;
	req.push_back("todos");
	tools.push_back(_make_tool("update_todos", "Update the task list displayed to the user. Full replace — send the complete list each call.", props, req));
}
