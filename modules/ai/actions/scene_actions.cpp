// modules/ai/actions/scene_actions.cpp
// Scene-related action implementations for the AI module

#include "scene_actions.h"
#include "action_common.h"

#include "../ai.h" // For AI::get_singleton() (read-before-edit gating)

#include "editor/editor_interface.h"
#include "editor/editor_command_palette.h"
#include "editor/editor_file_system.h"
#include "editor/editor_node.h"
#include "editor/editor_paths.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/gui/editor_run_bar.h"
#include "core/string/ustring.h"
#include "core/object/class_db.h"
#include "scene/resources/packed_scene.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_saver.h"
#include "core/io/resource_uid.h"
#include "scene/main/node.h"
#include "core/config/project_settings.h"
#include "core/io/resource_loader.h"
#include "core/os/os.h"

namespace AISceneActions {

#ifdef TOOLS_ENABLED

// Returns the index of the open scene tab whose path matches, or -1 if the scene is not open.
static int _find_open_scene_tab(const String &p_res_path) {
	EditorData &editor_data = EditorNode::get_editor_data();
	for (int i = 0; i < editor_data.get_edited_scene_count(); i++) {
		if (editor_data.get_scene_path(i) == p_res_path) {
			return i;
		}
	}
	return -1;
}

// Returns true if the scene tab at the given index has unsaved changes.
static bool _is_scene_tab_unsaved(int p_tab_idx) {
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	if (!undo_redo) {
		return false;
	}
	int history_id = EditorNode::get_editor_data().get_scene_history_id(p_tab_idx);
	return undo_redo->is_history_unsaved(history_id);
}

// Extracts an attribute value like `uid="..."` from within a tag substring.
// The attribute key is matched with a leading space to avoid substring collisions.
static String _extract_tag_attribute(const String &p_tag, const String &p_key) {
	String needle = " " + p_key + "=\"";
	int start = p_tag.find(needle);
	if (start == -1) {
		return String();
	}
	start += needle.length();
	int end = p_tag.find("\"", start);
	if (end == -1) {
		return String();
	}
	return p_tag.substr(start, end - start);
}

// Extracts the uid from the [gd_scene ...] header, or "" if absent.
static String _extract_header_uid(const String &p_content) {
	int header_start = p_content.find("[gd_scene");
	if (header_start == -1) {
		return String();
	}
	int header_end = p_content.find("]", header_start);
	if (header_end == -1) {
		return String();
	}
	return _extract_tag_attribute(p_content.substr(header_start, header_end - header_start), "uid");
}

// Scans the [ext_resource ...] tags in the header region of a .tscn (everything before the
// first [sub_resource]/[node] tag — ext_resources cannot legally appear later, and stopping
// there avoids false positives from string properties that contain tag-like lines).
// Missing paths are fatal (the editor loads with abort_on_missing_resources=false, so the
// validation load would NOT catch them). A registered uid pointing at a different path is a
// warning: at load time the uid wins over the path.
static void _check_ext_resources(const String &p_content, Array &r_missing, Array &r_warnings) {
	Vector<String> lines = p_content.split("\n");
	for (int i = 0; i < lines.size(); i++) {
		String line = lines[i].strip_edges();
		if (line.begins_with("[sub_resource") || line.begins_with("[node")) {
			break;
		}
		if (!line.begins_with("[ext_resource")) {
			continue;
		}
		String path = _extract_tag_attribute(line, "path");
		if (path.is_empty()) {
			continue; // Malformed tag — the validation load will report it.
		}
		if (!ResourceLoader::exists(path)) {
			r_missing.push_back(path);
			continue;
		}
		String uid_text = _extract_tag_attribute(line, "uid");
		if (!uid_text.is_empty()) {
			ResourceUID::ID uid = ResourceUID::get_singleton()->text_to_id(uid_text);
			if (uid != ResourceUID::INVALID_ID && ResourceUID::get_singleton()->has_id(uid)) {
				String uid_path = ResourceUID::get_singleton()->get_id_path(uid);
				if (uid_path != path) {
					r_warnings.push_back(vformat(
							"ext_resource uid '%s' is registered to '%s', not '%s'. The uid takes precedence over the path at load time — this reference will load '%s'. Update the uid attribute if you meant to repoint the reference.",
							uid_text, uid_path, path, uid_path));
				}
			}
		}
	}
}

// Collects error text emitted during the validation load (ResourceLoaderText reports
// parse errors as "path:line - Parse Error: msg" through the global error handler chain).
struct AISceneLoadErrorCapture {
	Vector<String> lines;
};

static void _ai_scene_load_error_handler(void *p_userdata, const char *p_function, const char *p_file, int p_line, const char *p_error, const char *p_message, bool p_editor_notify, ErrorHandlerType p_type) {
	if (p_type != ERR_HANDLER_ERROR && p_type != ERR_HANDLER_SCRIPT) {
		return;
	}
	AISceneLoadErrorCapture *capture = static_cast<AISceneLoadErrorCapture *>(p_userdata);
	String msg = String::utf8(p_error);
	String explanation = String::utf8(p_message);
	if (!explanation.is_empty() && explanation != msg) {
		msg += " (" + explanation + ")";
	}
	capture->lines.push_back(msg);
}

static int _count_occurrences(const String &p_content, const String &p_needle) {
	int count = 0;
	int search_from = 0;
	while (true) {
		int pos = p_content.find(p_needle, search_from);
		if (pos == -1) {
			break;
		}
		count++;
		search_from = pos + p_needle.length();
	}
	return count;
}

#endif // TOOLS_ENABLED

Dictionary exec_read_scene_file(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	String file_path = args["file_path"];

	if (!file_path.begins_with("res://")) {
		return ai_create_error_result(AIErrorCodes::INVALID_PATH,
			vformat("file_path must start with 'res://'. Got: '%s'", file_path));
	}
	if (!file_path.to_lower().ends_with(".tscn")) {
		return ai_create_error_result(AIErrorCodes::INVALID_TYPE,
			vformat("Only .tscn scene files are supported. Got: '%s'", file_path));
	}
	if (!FileAccess::exists(file_path)) {
		return ai_create_error_result(AIErrorCodes::FILE_NOT_FOUND,
			vformat("Scene file does not exist at '%s'", file_path));
	}

	Ref<FileAccess> file = FileAccess::open(file_path, FileAccess::READ);
	if (file.is_null()) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("Failed to open scene file for reading at '%s'", file_path));
	}
	String content = file->get_as_text();
	file.unref();

	int tab_idx = _find_open_scene_tab(file_path);
	bool dirty = tab_idx >= 0 && _is_scene_tab_unsaved(tab_idx);

	Dictionary result_data;
	result_data["file_path"] = file_path;
	result_data["content"] = content;
	result_data["size"] = content.length();
	result_data["is_open_in_editor"] = tab_idx >= 0;
	result_data["has_unsaved_changes"] = dirty;
	if (dirty) {
		result_data["warning"] = "This scene is open in the editor with UNSAVED changes — the disk content above is STALE. Do not edit this file until the scene is saved (the user can save it, or you can call save_scene while it is the active scene).";
	}

	print_line(vformat("AI: Executed read_scene_file. File: %s (%d bytes)", file_path, content.length()));
	return ai_create_success_result(result_data);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

Dictionary exec_update_scene_file(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	AI *ai_singleton = AI::get_singleton();
	if (!ai_singleton) {
		return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			"AI singleton not found");
	}

	String file_path = args["file_path"];
	String old_string = args["old_string"];
	String new_string = args["new_string"];
	bool replace_all = args.get("replace_all", false);

	if (!file_path.begins_with("res://")) {
		return ai_create_error_result(AIErrorCodes::INVALID_PATH,
			vformat("file_path must start with 'res://'. Got: '%s'", file_path));
	}
	if (!file_path.to_lower().ends_with(".tscn")) {
		return ai_create_error_result(AIErrorCodes::INVALID_TYPE,
			vformat("Only .tscn scene files are supported. Got: '%s'", file_path));
	}
	if (old_string == new_string) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"old_string and new_string are identical — nothing to change.");
	}

	String abs_path = ProjectSettings::get_singleton()->globalize_path(file_path);
	if (!FileAccess::exists(abs_path)) {
		return ai_create_error_result(AIErrorCodes::FILE_NOT_FOUND,
			vformat("Scene file does not exist at '%s'", file_path));
	}

	// Require read_scene_file before update_scene_file (prevents blind edits)
	if (!ai_singleton->was_scene_file_read(file_path)) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			vformat("You must call read_scene_file on '%s' before updating it. Read the file first to understand its current contents.", file_path));
	}

	// Check if game is running (scene may be loaded by the running process; reloading mid-play is unsafe)
	EditorRunBar *run_bar = EditorRunBar::get_singleton();
	if (run_bar && run_bar->is_playing()) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			"Cannot update a scene file while the game is running. Stop the game (F8) first.");
	}

	// Refuse when the scene tab has unsaved changes — editing the file on disk would discard them.
	int tab_idx = _find_open_scene_tab(file_path);
	if (tab_idx >= 0 && _is_scene_tab_unsaved(tab_idx)) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("Scene '%s' is open in the editor with unsaved changes. Editing the file on disk would discard them. Ask the user to save the scene (or call save_scene while it is the active scene), then call read_scene_file again and retry.", file_path));
	}

	Ref<FileAccess> file = FileAccess::open(abs_path, FileAccess::READ);
	if (file.is_null()) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("Failed to open scene file for reading at '%s'", file_path));
	}
	String original_content = file->get_as_text();
	file.unref();

	// Normalize CRLF to LF (git autocrlf can produce CRLF working copies; the model sends LF;
	// Godot's own scene saver writes LF, so resaving normalized matches editor behavior).
	bool line_endings_normalized = false;
	if (original_content.contains("\r\n")) {
		original_content = original_content.replace("\r\n", "\n");
		line_endings_normalized = true;
	}
	old_string = old_string.replace("\r\n", "\n");
	new_string = new_string.replace("\r\n", "\n");

	// Count occurrences — raw match first. Unlike update_script, escape normalization is only a
	// fallback: .tscn quoted strings legitimately contain literal \n / \" two-char sequences
	// (e.g. text = "Hello\nWorld"), so unconditional unescaping would corrupt exact matches.
	int occurrence_count = _count_occurrences(original_content, old_string);
	if (occurrence_count == 0) {
		String old_norm = old_string.replace("\\n", "\n").replace("\\t", "\t").replace("\\\"", "\"");
		if (old_norm != old_string) {
			int norm_count = _count_occurrences(original_content, old_norm);
			if (norm_count > 0) {
				occurrence_count = norm_count;
				old_string = old_norm;
				new_string = new_string.replace("\\n", "\n").replace("\\t", "\t").replace("\\\"", "\"");
			}
		}
	}

	if (occurrence_count == 0) {
		Dictionary details;
		details["file_size"] = original_content.length();
		if (original_content.length() <= 32768) {
			details["current_file_content"] = original_content;
			return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
				vformat("old_string not found in '%s'. The current file content is included in details — use it to construct an exact match.", file_path),
				details);
		}
		details["hint"] = "File too large to include in this error — call read_scene_file again to refresh your view of it.";
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			vformat("old_string not found in '%s'.", file_path),
			details);
	}

	if (occurrence_count > 1 && !replace_all) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			vformat("old_string matches %d locations in '%s'. Provide more surrounding context to make it unique, or set replace_all to true.", occurrence_count, file_path));
	}

	// Perform the replacement
	String new_content;
	if (replace_all) {
		new_content = original_content.replace(old_string, new_string);
	} else {
		int pos = original_content.find(old_string);
		new_content = original_content.substr(0, pos) + new_string + original_content.substr(pos + old_string.length());
	}

	// The scene's own uid must survive the edit — references elsewhere resolve through it.
	if (_extract_header_uid(new_content) != _extract_header_uid(original_content)) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"This edit would change or remove the uid in the [gd_scene] header. The scene's uid must be preserved exactly — construct old_string/new_string so the header uid is untouched.");
	}

	// Missing ext_resource paths would NOT fail the validation load (the editor loads with
	// abort_on_missing_resources=false), so check them explicitly.
	Array missing_ext_resources;
	Array warnings;
	_check_ext_resources(new_content, missing_ext_resources, warnings);
	if (!missing_ext_resources.is_empty()) {
		Dictionary details;
		details["missing_ext_resources"] = missing_ext_resources;
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			vformat("The edited scene references %d ext_resource path(s) that do not exist. The file on disk was NOT modified. Create the missing resources first or fix the paths.", missing_ext_resources.size()),
			details);
	}

	// Validate by loading a temp copy from outside res:// (never scanned, uid not registered).
	String temp_path = EditorPaths::get_singleton()->get_temp_dir().path_join(
			vformat("ai_update_scene_%s.tscn", itos(OS::get_singleton()->get_process_id())));
	{
		Ref<FileAccess> temp_file = FileAccess::open(temp_path, FileAccess::WRITE);
		if (temp_file.is_null()) {
			return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
				vformat("Failed to write validation temp file at '%s'.", temp_path));
		}
		temp_file->store_string(new_content);
	}

	AISceneLoadErrorCapture capture;
	ErrorHandlerList error_handler;
	error_handler.errfunc = _ai_scene_load_error_handler;
	error_handler.userdata = &capture;
	add_error_handler(&error_handler);

	Error load_err = OK;
	Ref<Resource> validated = ResourceLoader::load(temp_path, "PackedScene",
			ResourceFormatLoader::CACHE_MODE_IGNORE, &load_err);

	remove_error_handler(&error_handler);
	DirAccess::remove_absolute(temp_path);

	if (load_err != OK || validated.is_null()) {
		Dictionary details;
		if (!capture.lines.is_empty()) {
			Array load_errors;
			for (int i = 0; i < capture.lines.size(); i++) {
				load_errors.push_back(capture.lines[i]);
			}
			details["load_errors"] = load_errors;
		}
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("The edited scene failed to load (error %d). The file on disk was NOT modified. Fix the edit and retry — see 'load_errors' in details.", load_err),
			details);
	}
	validated.unref();

	// Commit: write the real file.
	{
		Ref<FileAccess> out_file = FileAccess::open(abs_path, FileAccess::WRITE);
		if (out_file.is_null()) {
			return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
				vformat("Failed to open scene file for writing at '%s'", file_path));
		}
		out_file->store_string(new_content);
	}

	// Refresh the filesystem cache for this file.
	EditorFileSystem *efs = EditorFileSystem::get_singleton();
	if (efs) {
		efs->update_file(file_path);
	}

	// Reload the tab if the scene is open. Guard is required: reload_scene() on a scene with
	// no tab clears the CURRENT scene's undo history instead.
	bool reloaded = false;
	if (tab_idx >= 0) {
		EditorNode *editor_node = EditorNode::get_singleton();
		if (editor_node) {
			editor_node->reload_scene(file_path);
			reloaded = true;
		}
	}

	Dictionary result_data;
	result_data["file_path"] = file_path;
	result_data["replacements"] = replace_all ? occurrence_count : 1;
	result_data["old_size"] = original_content.length();
	result_data["new_size"] = new_content.length();
	result_data["was_open_in_editor"] = tab_idx >= 0;
	result_data["reloaded_in_editor"] = reloaded;
	if (line_endings_normalized) {
		result_data["line_endings_normalized"] = true;
	}
	if (!warnings.is_empty()) {
		result_data["warnings"] = warnings;
	}
	result_data["note"] = "Scene file edits are NOT undoable with Ctrl+Z. The editor tab (if open) was reloaded from disk and its undo history cleared.";

	print_line(vformat("AI: Executed update_scene_file. File: %s, %d replacement(s)", file_path, replace_all ? occurrence_count : 1));
	return ai_create_success_result(result_data);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

Dictionary exec_open_scene(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			"EditorInterface singleton not found");
	}

	String scene_path = args["scene_path"];
	if (scene_path.is_empty()) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"scene_path is empty");
	}

	ei->open_scene_from_path(scene_path);

	Dictionary result_data;
	result_data["scene_path"] = scene_path;

	print_line(vformat("AI: Executed open_scene. Path: %s", scene_path));
	return ai_create_success_result(result_data);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

Dictionary exec_create_scene(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			"EditorInterface singleton not found");
	}

	String scene_path = args["scene_path"];
	if (scene_path.is_empty()) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"scene_path is empty");
	}

	String root_type = args.get("root_type", "Node");
	String root_name = args.get("root_name", "Main");

	// Validate root_type and fall back to Node if invalid
	if (!ClassDB::class_exists(StringName(root_type))) {
		ai_log_error(vformat("Execute 'create_scene': Invalid root_type '%s', falling back to 'Node'.", root_type));
		root_type = "Node";
	}

	// Create root node instance
	Node *root_node = Object::cast_to<Node>(ClassDB::instantiate(StringName(root_type)));
	if (!root_node) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("Failed to instantiate node of type '%s'", root_type));
	}

	root_node->set_name(root_name);

	// Pack into PackedScene
	Ref<PackedScene> packed_scene;
	packed_scene.instantiate();
	packed_scene->pack(root_node);

	// Save to scene_path
	Error err = ResourceSaver::save(packed_scene, scene_path);
	if (err != OK) {
		root_node->queue_free();
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("Failed to save scene to '%s'. Error: %d", scene_path, err));
	}

	// Open the scene in the editor
	ei->open_scene_from_path(scene_path);

	Dictionary result_data;
	result_data["scene_path"] = scene_path;
	result_data["root_type"] = root_type;
	result_data["root_name"] = root_name;

	print_line(vformat("AI: Executed create_scene. Path: %s, RootType: %s, RootName: %s", scene_path, root_type, root_name));
	return ai_create_success_result(result_data);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

Dictionary exec_save_scene(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			"EditorInterface singleton not found");
	}

	EditorCommandPalette *command_palette = ei->get_command_palette();
	if (!command_palette) {
		return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			"EditorCommandPalette not found");
	}

	command_palette->execute_command("editor/save_scene");

	Dictionary result_data;
	// Note: We don't have direct access to the saved scene path here,
	// but the command executes successfully

	print_line("AI: Executed save_scene.");
	return ai_create_success_result(result_data);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

Dictionary exec_set_main_scene(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	String scene_path = args.get("scene_path", String());
	if (scene_path.is_empty()) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"scene_path is empty");
	}

	// Validate that the scene resource exists.
	if (!ResourceLoader::exists(scene_path)) {
		return ai_create_error_result(AIErrorCodes::FILE_NOT_FOUND,
			vformat("Scene file does not exist at path '%s'", scene_path));
	}

	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			"ProjectSettings singleton not available");
	}

	const String setting_key = "application/run/main_scene";
	String old_main_scene = ps->get_setting(setting_key, String());
	ps->set_setting(setting_key, scene_path);

	Error err = ps->save();
	if (err != OK) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("Failed to save ProjectSettings (project.godot). Error: %d", err));
	}

	Dictionary result_data;
	result_data["scene_path"] = scene_path;
	result_data["previous_main_scene"] = old_main_scene;

	print_line(vformat("AI: Executed set_main_scene. Main scene set to: %s", scene_path));
	return ai_create_success_result(result_data);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

Dictionary exec_close_scene(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			"EditorInterface singleton not found");
	}

	EditorNode *editor_node = EditorNode::get_singleton();
	if (!editor_node) {
		return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			"EditorNode singleton not found");
	}

	bool save_if_modified = args.get("save_if_modified", true);

	// Check if there's an edited scene
	Node *edited_scene_root = ai_get_edited_scene_root();
	Dictionary result_data;
	result_data["save_if_modified"] = save_if_modified;

	if (!edited_scene_root) {
		ai_log_verbose("Execute 'close_scene': No edited scene to close.");
		result_data["was_no_op"] = true;
		return ai_create_success_result(result_data); // No scene open, consider it successful
	}

	// Check if scene has unsaved changes
	bool has_unsaved_changes = false;
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	if (undo_redo) {
		EditorData &editor_data = EditorNode::get_editor_data();
		int current_scene = editor_data.get_edited_scene();
		int history_id = editor_data.get_scene_history_id(current_scene);
		has_unsaved_changes = undo_redo->is_history_unsaved(history_id);
	}

	result_data["had_unsaved_changes"] = has_unsaved_changes;
	result_data["was_no_op"] = false;

	// If save_if_modified is true and there are unsaved changes, save first
	if (save_if_modified && has_unsaved_changes) {
		EditorCommandPalette *command_palette = ei->get_command_palette();
		if (command_palette) {
			command_palette->execute_command("editor/save_scene");
			ai_log_verbose("Execute 'close_scene': Saved scene before closing.");
			result_data["saved_before_close"] = true;
		} else {
			return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
				"EditorCommandPalette not found for saving");
		}
	} else {
		result_data["saved_before_close"] = false;
	}

	// Close the scene using EditorNode's menu option
	// The second parameter (true) means "confirmed" - skip confirmation dialogs
	editor_node->trigger_menu_option(EditorNode::FILE_CLOSE, true);

	print_line("AI: Executed close_scene.");
	return ai_create_success_result(result_data);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

Dictionary exec_list_open_scenes(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED, "EditorInterface not available");
	}

	PackedStringArray open_paths = ei->get_open_scenes();
	Node *edited_root = ei->get_edited_scene_root();
	String current_scene = edited_root ? edited_root->get_scene_file_path() : String();

	Array scenes;
	for (int i = 0; i < open_paths.size(); i++) {
		Dictionary entry;
		entry["path"] = open_paths[i];
		entry["is_current"] = (open_paths[i] == current_scene);
		scenes.push_back(entry);
	}

	Dictionary result_data;
	result_data["scenes"] = scenes;
	result_data["count"] = scenes.size();
	result_data["current_scene"] = current_scene;
	return ai_create_success_result(result_data);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

Dictionary exec_stop_game(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	EditorRunBar *run_bar = EditorRunBar::get_singleton();
	if (!run_bar) {
		return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			"EditorRunBar singleton not found");
	}

	bool was_playing = run_bar->is_playing();
	Dictionary result_data;
	result_data["was_playing"] = was_playing;

	if (was_playing) {
		run_bar->stop_playing();
		print_line("AI: Executed stop_game.");
	} else {
		ai_log_verbose("Execute 'stop_game': Game was not running, no-op.");
	}

	return ai_create_success_result(result_data);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

} // namespace AISceneActions

