// tools_array.inc — Native tool definitions for Aristotle AI module
// Included by ai_provider.cpp inside AIProvider::build_tools_array()
// Each block pushes one tool definition into the `tools` Array.
// Uses _make_prop() and _make_tool() helpers defined in ai_provider.cpp.

// ==========================================================================
// Node actions
// ==========================================================================
// Shared description for the optional scene_path accepted by all node-mutating tools.
// Mutations always apply to the ACTIVE scene tab, so naming another open scene switches to it.
const String mutation_scene_path_desc = "Scene to modify, e.g. 'res://main.tscn' (optional). Must be a currently OPEN scene tab; if it is not the active tab, the editor switches to it before applying the change. Omit to use the active scene. IMPORTANT: without this, the change goes to whichever scene tab is active — which may not be the scene you last read.";
{
	Dictionary props;
	props["node_name"] = _make_prop("string", "Name for the new node");
	props["node_type"] = _make_prop("string", "Godot node type (e.g. CharacterBody2D, Sprite2D, Camera2D)");
	props["parent_path"] = _make_prop("string", "Path relative to scene root. Omit for root child.");
	props["scene_path"] = _make_prop("string", mutation_scene_path_desc);
	Array req;
	req.push_back("node_name");
	req.push_back("node_type");
	tools.push_back(_make_tool("create_node", "Create a new node in the scene tree", props, req));
}
{
	Dictionary props;
	props["node_path"] = _make_prop("string", "Path of the node to delete (relative to scene root)");
	props["scene_path"] = _make_prop("string", mutation_scene_path_desc);
	Array req;
	req.push_back("node_path");
	tools.push_back(_make_tool("delete_node", "Delete a node from the scene tree", props, req));
}
{
	Dictionary props;
	props["node_path"] = _make_prop("string", "Path of the node to duplicate");
	props["new_name"] = _make_prop("string", "Name for the duplicate (optional, defaults to <name>_copy)");
	props["scene_path"] = _make_prop("string", mutation_scene_path_desc);
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
	value_prop["description"] = "The value to set. Pass raw values — strings as \"hello\", numbers as 1.0, bools as true. For Vector2/Vector3/Color use arrays: [x,y] or [x,y,z]. Do NOT wrap values in {type, value} objects.";
	props["value"] = value_prop;
	props["scene_path"] = _make_prop("string", mutation_scene_path_desc);
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
	props["scene_path"] = _make_prop("string", mutation_scene_path_desc);
	Array req;
	req.push_back("node_path");
	req.push_back("property_name");
	req.push_back("resource_type");
	tools.push_back(_make_tool("create_resource", "Create a new Resource and assign it to a node property", props, req));
}
{
	Dictionary props;
	props["node_path"] = _make_prop("string", "AnimatedSprite2D or AnimatedSprite3D node to receive the SpriteFrames (path relative to scene root). Optional when save_path is given; at least one of node_path / save_path is required.");
	props["save_path"] = _make_prop("string", "Optional resource file path ending in .tres (e.g. res://animations/eyes.tres). When given, the SpriteFrames is saved to disk as a standalone resource; if node_path is also given, the node references the saved file instead of embedding it. Omit for a scene-embedded resource (the default, and fully undoable).");
	props["scene_path"] = _make_prop("string", mutation_scene_path_desc);

	Dictionary anims_prop;
	anims_prop["type"] = "array";
	anims_prop["description"] = "Animations to create in the SpriteFrames resource. Define ALL animations in one call (e.g. 'blink' and 'talk' together) — the call REPLACES any existing SpriteFrames on the node.";
	Dictionary anim_items;
	anim_items["type"] = "object";
	Dictionary anim_props;
	anim_props["name"] = _make_prop("string", "Animation name (e.g. 'idle', 'blink', 'talk'). Must be unique within this call.");
	anim_props["fps"] = _make_prop("number", "Playback speed in frames per second (default 5.0, must be > 0).");
	anim_props["loop"] = _make_prop("boolean", "Whether the animation loops (default true). Use false for one-shots like 'blink'.");
	Dictionary frames_prop;
	frames_prop["type"] = "array";
	frames_prop["description"] = "Ordered frames of this animation. The same texture may appear in multiple frames or animations.";
	Dictionary frame_items;
	frame_items["type"] = "object";
	Dictionary frame_props;
	frame_props["texture"] = _make_prop("string", "Image asset path (res://...). Must be an already-imported texture (png/jpg/webp/svg).");
	frame_props["duration"] = _make_prop("number", "Relative duration multiplier for this frame (default 1.0, must be > 0). At 5 fps, duration 2.0 shows the frame for 0.4s instead of 0.2s.");
	Dictionary region_prop;
	region_prop["type"] = "array";
	Dictionary region_items;
	region_items["type"] = "number";
	region_prop["items"] = region_items;
	region_prop["description"] = "Optional [x, y, width, height] pixel rect to cut this frame out of a sprite-sheet texture (wraps it in an AtlasTexture). Omit to use the whole image.";
	frame_props["region"] = region_prop;
	frame_items["properties"] = frame_props;
	Array frame_req;
	frame_req.push_back("texture");
	frame_items["required"] = frame_req;
	frames_prop["items"] = frame_items;
	anim_props["frames"] = frames_prop;
	anim_items["properties"] = anim_props;
	Array anim_req;
	anim_req.push_back("name");
	anim_req.push_back("frames");
	anim_items["required"] = anim_req;
	anims_prop["items"] = anim_items;
	props["animations"] = anims_prop;

	Array req;
	req.push_back("animations");
	tools.push_back(_make_tool("create_sprite_frames",
			"Build a fully-populated SpriteFrames resource — multiple named animations, per-frame textures, durations, fps, loop — and assign it to an AnimatedSprite2D/AnimatedSprite3D's sprite_frames slot in ONE declarative call. ALWAYS prefer this over scripted frame-swapping (texture-swap timers/coroutines, per-pixel ColorRects) for frame-by-frame animation. Replaces the node's existing SpriteFrames entirely (undoable), so re-issue the full desired state to modify. All texture paths are validated before anything changes; on error nothing is mutated. After assignment the node's current animation is set automatically; set the node's 'autoplay' property with set_property if it should play on scene start.",
			props, req));
}
{
	Dictionary props;
	props["node_path"] = _make_prop("string", "Path of the node to rename");
	props["new_name"] = _make_prop("string", "New name for the node");
	props["scene_path"] = _make_prop("string", mutation_scene_path_desc);
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
	props["scene_path"] = _make_prop("string", mutation_scene_path_desc);
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
	props["scene_path"] = _make_prop("string", mutation_scene_path_desc);
	Array req;
	req.push_back("node_path");
	req.push_back("script_path");
	tools.push_back(_make_tool("attach_script", "Attach a script to a node", props, req));
}
{
	Dictionary props;
	props["node_path"] = _make_prop("string", "Path of the node to detach the script from");
	props["scene_path"] = _make_prop("string", mutation_scene_path_desc);
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
	props["scene_path"] = _make_prop("string", mutation_scene_path_desc);
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
	props["scene_path"] = _make_prop("string", mutation_scene_path_desc);
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
{
	Dictionary props;
	props["file_path"] = _make_prop("string", "Scene file path to read (e.g. res://scenes/main.tscn). Must be a .tscn file.");
	Array req;
	req.push_back("file_path");
	tools.push_back(_make_tool("read_scene_file", "Read the raw text content of a .tscn scene file from disk. Check 'has_unsaved_changes' in the result: if true, the scene is open in the editor with unsaved changes and the disk content is STALE — the scene must be saved before you can edit the file with update_scene_file.", props, req));
}
{
	Dictionary props;
	props["file_path"] = _make_prop("string", "Path of the .tscn scene file to update (e.g. res://scenes/main.tscn)");
	props["old_string"] = _make_prop("string", "The exact text to find and replace");
	props["new_string"] = _make_prop("string", "The replacement text (must differ from old_string)");
	props["replace_all"] = _make_prop("boolean", "Replace all occurrences (default false)");
	Array req;
	req.push_back("file_path");
	req.push_back("old_string");
	req.push_back("new_string");
	tools.push_back(_make_tool("update_scene_file", "Performs exact string replacement in a .tscn scene file on disk. You MUST have called read_scene_file on this file first — this tool will fail otherwise. Fails if old_string is not found or is not unique (provide more surrounding context, or set replace_all to true). Fails if the scene is open in the editor with unsaved changes — the user must save first. Never change the uid in the [gd_scene] header — such edits are rejected. The edited scene is validated by loading it BEFORE committing: on failure the file on disk is left untouched (check 'load_errors' in the error details). If the scene is open in a tab it is reloaded from disk automatically. These edits are NOT undoable with Ctrl+Z — prefer node tools (create_node, set_property, reparent_node) for small tweaks; use this for bulk or structural changes that would take many node-tool calls.", props, req));
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
	props["source_path"] = _make_prop("string", "Path to the source file. Use res:// if the file is already in the project; use an absolute OS path (e.g. C:\\Users\\You\\Downloads\\file.png) for files outside the project.");
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
{
	Dictionary props;
	props["source_path"] = _make_prop("string", "Source file path (res://...). Must exist.");
	props["dest_path"]   = _make_prop("string", "Destination file path (res://...). Parent dir is created if missing.");
	props["overwrite"]   = _make_prop("boolean", "Overwrite if destination exists (default false)");
	Array req;
	req.push_back("source_path");
	req.push_back("dest_path");
	tools.push_back(_make_tool("copy_file",
		"Duplicate any file in the project (scripts, images, scenes, resources). "
		"Handles .import sidecars and regenerates UIDs so the copy is editor-correct. "
		"Source and destination are both res:// paths.",
		props, req));
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
	props["scene_path"] = _make_prop("string", "Scene to run (res:// path, optional). Defaults to the project's main scene. Use this to test a specific scene without changing the project's main scene setting.");
	props["wait_seconds"] = _make_prop("number", "Seconds to let the game run before capturing a single screenshot (default 2.0). Ignored when screenshot_times_seconds is provided.");
	Dictionary times_prop;
	times_prop["type"] = "array";
	Dictionary times_items;
	times_items["type"] = "number";
	times_prop["items"] = times_items;
	times_prop["description"] = "Optional list of capture times in seconds, measured from the moment the game starts running, e.g. [1.0, 3.0, 5.0]. Times are sorted and deduped. Total wait is bounded by the largest entry. When set, multiple screenshots are returned as result.screenshots = [{at_seconds, screenshot_b64}, ...] instead of the single-shot result.screenshot_b64.";
	props["screenshot_times_seconds"] = times_prop;
	Array req;
	tools.push_back(_make_tool("run_and_screenshot", "Run the game briefly, capture one or more screenshots at specific times, then stop. The screenshots are returned as images you can see — use them to visually verify layout, colors, animation progression, and positioning over time. Inform the user if there is an issue with viewing the images. Prefer capture_2d_viewport / capture_3d_viewport for static visual checks. Note that those work best when the majority of the rendering is not dependent on runtime scripts.", props, req));
}
{
	Dictionary props;
	Dictionary frame_rect_prop;
	frame_rect_prop["type"] = "array";
	frame_rect_prop["description"] = "Optional world-space rectangle [x, y, width, height] to frame in the capture (units match node positions in the scene; e.g. a Sprite2D at position (100, 100) lives in the same coordinate system). Example: frame_rect: [50, 50, 200, 200] frames a 200x200 world region starting at (50, 50). Fits the rect into the viewport preserving aspect ratio (may produce empty bands if the rect's aspect differs from the viewport's). Omit to use the editor's current pan/zoom.";
	Dictionary frame_rect_items;
	frame_rect_items["type"] = "number";
	frame_rect_prop["items"] = frame_rect_items;
	props["frame_rect"] = frame_rect_prop;
	Array req;
	tools.push_back(_make_tool("capture_2d_viewport", "Capture the 2D editor viewport as an image. Use to visually check the 2D scene the user is editing (sprites, UI, layout) without running the game. Much faster than run_and_screenshot for quick visual checks, and shows exactly what the user sees in the 2D canvas (including their current pan/zoom — unless you pass frame_rect to override the framing). The image is returned and shown to you automatically. The result includes world_rect ([x, y, width, height] of the world-space region the image spans) and px_per_world_unit — use them to convert image pixels to world/node coordinates; without frame_rect the editor's view scale means image pixels are usually NOT 1:1 with world units.", props, req));
}
{
	Dictionary props;
	Dictionary shot_pos_prop;
	shot_pos_prop["type"] = "array";
	shot_pos_prop["description"] = "Optional world-space position [x, y, z] the shot is taken FROM (units match node global_position). If provided, shot_target must also be provided. This is an input to this capture only — it is NOT a Camera3D node in the scene, and changing any Camera3D node in the scene does not change this shot. To reframe, vary shot_position/shot_target directly; repeating the same values gives the same picture.";
	Dictionary shot_pos_items;
	shot_pos_items["type"] = "number";
	shot_pos_prop["items"] = shot_pos_items;
	props["shot_position"] = shot_pos_prop;

	Dictionary shot_tgt_prop;
	shot_tgt_prop["type"] = "array";
	shot_tgt_prop["description"] = "Optional world-space point [x, y, z] the shot looks AT (up vector fixed to Y+). Must be paired with shot_position. Distance between shot_position and shot_target controls apparent zoom: pull shot_position further from shot_target for a wider/farther view, closer for a tighter view. Example: shot_position: [5, 5, 5], shot_target: [0, 0, 0] shoots the origin from an upper-right angle. Avoid placing target directly above/below position (Y-axis gimbal); use a small X/Z offset.";
	Dictionary shot_tgt_items;
	shot_tgt_items["type"] = "number";
	shot_tgt_prop["items"] = shot_tgt_items;
	props["shot_target"] = shot_tgt_prop;

	props["shot_fov"] = _make_prop("number", "Optional vertical field of view in degrees for the shot (default 70, range 0-180 exclusive). Only used when shot_position/shot_target are provided.");

	Dictionary size_prop;
	size_prop["type"] = "array";
	size_prop["description"] = "Optional output image size [width, height] in pixels (default [1280, 720], minimum 64 per axis). Only used when shot_position/shot_target are provided — the default (editor-viewport) path returns the editor viewport's native size.";
	Dictionary size_items;
	size_items["type"] = "number";
	size_prop["items"] = size_items;
	props["size"] = size_prop;

	Array req;
	tools.push_back(_make_tool("capture_3d_viewport", "Capture the 3D editor viewport as an image. Use to visually check the 3D scene the user is editing (meshes, angle, lighting) without running the game. By default captures the 3D viewport the user most recently interacted with (with their current orbit view). Pass shot_position + shot_target to shoot the scene from an arbitrary viewpoint instead — useful for 'top-down view', 'look at this node from behind', or framing a region without asking the user to rotate the viewport. The shot_* parameters are pure inputs to this capture; they have nothing to do with any Camera3D node in the scene. If a previous shot came back unusable (blank, too close, wrong angle), vary shot_position/shot_target/shot_fov before retrying — repeating the same values produces the same picture. The image is returned and shown to you automatically.", props, req));
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
	props["scene_path"] = _make_prop("string", "Scene file to inspect, e.g. 'res://main.tscn' (optional). Must be a currently OPEN scene tab — does not load from disk. Defaults to the active scene.");
	Array req;
	tools.push_back(_make_tool("list_nodes", "List nodes in a scene tree. Defaults to the active scene; pass scene_path to inspect any other open scene without switching tabs.", props, req));
}
{
	Dictionary props;
	props["node_path"] = _make_prop("string", "Path of the node to inspect");
	props["resource_depth"] = _make_prop("integer", "Resource inspection depth (0=type only, 1=primitives, 2+=recurse, -1=unlimited; default 1)");
	props["scene_path"] = _make_prop("string", "Scene file the node lives in, e.g. 'res://main.tscn' (optional). Must be a currently OPEN scene tab. Defaults to the active scene.");
	Array req;
	req.push_back("node_path");
	tools.push_back(_make_tool("get_node_info", "Get detailed information about a single node (properties, sub_resources, warnings). Defaults to the active scene; pass scene_path to inspect a node in any other open scene.", props, req));
}
{
	Dictionary props;
	props["type_name"] = _make_prop("string", "Node type to search for (e.g. CharacterBody2D)");
	props["scene_path"] = _make_prop("string", "Scene file to search, e.g. 'res://main.tscn' (optional). Must be a currently OPEN scene tab. Defaults to the active scene.");
	Array req;
	req.push_back("type_name");
	tools.push_back(_make_tool("find_nodes_by_type", "Find all nodes of a specific type in a scene tree. Defaults to the active scene; pass scene_path to search any other open scene without switching tabs.", props, req));
}
{
	Dictionary props;
	props["directory"] = _make_prop("string", "Directory to list (e.g. res://scripts). Optional; defaults to res:// (project root).");
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
{
	Dictionary props;
	Dictionary paths_prop;
	paths_prop["type"] = "array";
	paths_prop["description"] = "Image file paths to preview (res://...)";
	Dictionary paths_items;
	paths_items["type"] = "string";
	paths_prop["items"] = paths_items;
	props["paths"] = paths_prop;
	props["max_size"] = _make_prop("integer", "Max dimension in px (default 128, range 32-256)");
	Array req;
	req.push_back("paths");
	tools.push_back(_make_tool("preview_asset",
		"Get low-res image previews. Returns visible thumbnails for each image file. "
		"Use after list_files to see what images contain before placing them. "
		"The thumbnail is DOWNSCALED — it is NOT the asset's real resolution. Each preview entry "
		"reports source_width/source_height (the file's true dimensions — use these for any "
		"layout or scale math) and preview_width/preview_height (the thumbnail).",
		props, req));
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

// ==========================================================================
// Export actions
// ==========================================================================
{
	Dictionary props;
	props["platform"] = _make_prop("string", "Optional platform name to filter results (e.g. 'Web')");
	Array req;
	tools.push_back(_make_tool("get_export_status",
		"Read-only overview of export readiness: available platforms, export presets with can-export verdicts, "
		"template installation state, and misconfiguration warnings the engine itself does NOT surface "
		"(selective export_filter with an empty file list, filter globs matching zero files — both silently "
		"produce a near-empty build). Call this FIRST when the user wants to export. Typical workflow: "
		"get_export_status -> create_export_preset -> (install_export_templates if missing) -> export_project; "
		"for web builds, serve_web_build to test locally. Export templates are a one-time install per engine version.",
		props, req));
}
{
	Dictionary props;
	props["platform"] = _make_prop("string", "Platform name, e.g. 'Web', 'Windows Desktop', 'Linux/X11', 'macOS', 'Android', 'iOS'. See get_export_status for the available list.");
	props["name"] = _make_prop("string", "Preset name (optional, defaults to the platform name)");
	props["export_path"] = _make_prop("string", "Output file path, project-relative or res:// (optional but recommended, e.g. 'builds/web/index.html' — extension must match the platform, .html for Web)");
	props["runnable"] = _make_prop("boolean", "Whether this preset is the platform's runnable/default preset (optional, default true; required true for serve_web_build)");
	props["export_filter"] = _make_prop("string", "Which resources to export: 'all_resources' (default), 'scenes', 'resources', 'exclude', 'customized'");
	Array req;
	req.push_back("platform");
	tools.push_back(_make_tool("create_export_preset",
		"Create an export preset for a platform with all option defaults filled in. Rewrites res://export_presets.cfg "
		"(not undoable). Prefer 'all_resources' export_filter unless the user asks otherwise — selective modes with an "
		"empty file list silently export a near-empty build.",
		props, req));
}
{
	Dictionary props;
	props["preset"] = _make_prop("string", "Name of the export preset to modify");
	props["option"] = _make_prop("string",
		"Option to set. Structural keys: name, export_path, runnable, export_filter (all_resources/scenes/resources/exclude/customized), "
		"include_filter, exclude_filter, export_files (array of res:// paths, full replace). Anything else is treated as a "
		"platform option (e.g. 'variant/thread_support', 'progressive_web_app/enabled') and validated against the preset's option list.");
	Dictionary value_prop;
	value_prop["description"] = "The value to set. Pass raw values — strings as \"hello\", numbers as 1.0, bools as true; export_files as an array of paths.";
	props["value"] = value_prop;
	Array req;
	req.push_back("preset");
	req.push_back("option");
	req.push_back("value");
	tools.push_back(_make_tool("set_export_preset_option",
		"Set one option on an existing export preset. Rewrites res://export_presets.cfg (not undoable, same as set_project_setting). "
		"IMPORTANT: include_filter/exclude_filter are comma-separated case-insensitive GLOBS matched against file paths "
		"(e.g. '*.txt, assets/*.json') — NOT lists of res:// resource paths; a bare path without wildcards matches nothing. "
		"Results include warnings when a filter setting would silently produce an empty or near-empty export.",
		props, req));
}
{
	Dictionary props;
	props["preset"] = _make_prop("string", "Name of the export preset to use");
	props["output_path"] = _make_prop("string", "Output file path (optional, defaults to the preset's export_path). Project-relative, res://, or absolute; extension must match the platform.");
	props["debug"] = _make_prop("boolean", "Export a debug build (optional, default false = release)");
	Array req;
	req.push_back("preset");
	tools.push_back(_make_tool("export_project",
		"Export the project using an existing preset (synchronous; seconds for small projects). Requires export templates "
		"(install_export_templates). The result includes the full export message log, the output file listing with sizes, "
		"and the pck's embedded file count — if that count is suspiciously low the export silently contains almost nothing "
		"(fix export_filter / export_files). Refuses to export a selective-filter preset with an empty file list.",
		props, req));
}
{
	Dictionary props;
	props["action"] = _make_prop("string", "'serve_and_open' (default: export, serve, open the user's browser), 'serve' (export + serve, return the URL only), or 'stop' (shut the server down)");
	props["preset"] = _make_prop("string", "Web preset to serve (optional, defaults to the first runnable Web preset)");
	Array req;
	tools.push_back(_make_tool("serve_web_build",
		"Export a temporary DEBUG web build and serve it on the editor's local HTTP server, which sends the COOP/COEP "
		"headers Godot web builds require — opening the exported .html directly or via a naive server will NOT boot. "
		"Returns the localhost URL. The served build is separate from export_project output. Requires a runnable Web "
		"preset and installed templates (debug included). The server keeps running until action 'stop' or the editor closes. "
		"This is the fastest way to let the user actually play a web build.",
		props, req));
}
{
	Dictionary props;
	props["force"] = _make_prop("boolean", "Reinstall even if templates are already present (optional, default false)");
	Array req;
	tools.push_back(_make_tool("install_export_templates",
		"Download and install the export templates matching this engine version (era-matched upstream package). "
		"One-time per engine version; required before any export. This is a LARGE background download (hundreds of MB, "
		"takes minutes) with progress shown to the user — call it ON ITS OWN, not batched with other tool calls. "
		"If templates are already installed it returns immediately.",
		props, req));
}
{
	Dictionary props;
	Array req;
	tools.push_back(_make_tool("open_template_manager",
		"Open the editor's Manage Export Templates dialog. NOTE: on this engine build the dialog's online download "
		"cannot find a matching package — use install_export_templates to install templates. Only useful for letting "
		"the user inspect or uninstall installed templates.",
		props, req));
}
