// modules/ai/actions/script_actions.cpp
// Script-related action implementations for the AI module

#include "script_actions.h"
#include "action_common.h"

#include "../ai.h" // For AI::get_singleton() and file helper methods

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/io/dir_access.h"
#include "core/io/resource_loader.h"
#include "core/object/script_language.h"
#include "scene/main/node.h"
#include "modules/gdscript/gdscript_parser.h"
#include "modules/gdscript/gdscript_analyzer.h"
#include "editor/gui/editor_run_bar.h"

namespace AIScriptActions {

Dictionary exec_create_script(const Dictionary &args) {
	EditorUndoRedoManager *undo_redo = ai_get_undo_redo();
	if (!undo_redo) {
		return ai_create_error_result(AIErrorCodes::NO_UNDO_REDO,
			"EditorUndoRedoManager singleton not found");
	}

	AI *ai_singleton = AI::get_singleton();
	if (!ai_singleton) {
		return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			"AI singleton not found");
	}

	String file_path = args["file_path"];
	String language = args["language"];
	String content = args["content"];

	// Validate language (v0: GDScript only)
	if (language != "GDScript") {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			vformat("Only 'GDScript' language is supported in v0. Got: '%s'", language));
	}

	// Convert to absolute path if it's a resource path
	String abs_path = ProjectSettings::get_singleton()->globalize_path(file_path);

	// Check if file already exists
	if (FileAccess::exists(abs_path)) {
		WARN_PRINT(vformat("AI Execute 'create_script': File already exists at '%s'. Skipping creation.", abs_path));
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("File already exists at '%s'", file_path));
	}

	ai_log_verbose(vformat("Creating script at '%s' with %d bytes of content", abs_path, content.length()));

	undo_redo->create_action("AI Create Script");
	undo_redo->add_do_method(ai_singleton, "_create_script_file", abs_path, content);
	undo_redo->add_undo_method(ai_singleton, "_delete_script_file", abs_path);
	undo_redo->commit_action();

	// Validate written script: parser + analyzer mirrors full editor error feedback.
	Array parse_errors;
	Array warnings;
	{
		GDScriptParser parser;
		Error parse_err = parser.parse(content, abs_path, false);

		for (const GDScriptParser::ParserError &e : parser.get_errors()) {
			Dictionary err_dict;
			err_dict["line"] = e.line;
			err_dict["column"] = e.column;
			err_dict["message"] = e.message;
			err_dict["type"] = "syntax";
			parse_errors.push_back(err_dict);
		}

		// Only run analyzer if parse succeeded — analyzer requires a valid parse tree
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
	result_data["language"] = language;
	result_data["size"] = content.length();
	if (!parse_errors.is_empty()) {
		result_data["parse_errors"] = parse_errors;
	}
	if (!warnings.is_empty()) {
		result_data["warnings"] = warnings;
	}

	print_line(vformat("AI: Executed create_script. File: %s, Language: %s", file_path, language));
	return ai_create_success_result(result_data);
}

Dictionary exec_update_script(const Dictionary &args) {
	EditorUndoRedoManager *undo_redo = ai_get_undo_redo();
	if (!undo_redo) {
		return ai_create_error_result(AIErrorCodes::NO_UNDO_REDO,
			"EditorUndoRedoManager singleton not found");
	}

	AI *ai_singleton = AI::get_singleton();
	if (!ai_singleton) {
		return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			"AI singleton not found");
	}

	String file_path = args["file_path"];
	String old_string = args["old_string"];
	String new_string = args["new_string"];
	bool replace_all = args.get("replace_all", false);

	if (old_string == new_string) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"old_string and new_string are identical — nothing to change.");
	}

	// Convert to absolute path if it's a resource path
	String abs_path = ProjectSettings::get_singleton()->globalize_path(file_path);

	// Check if file exists
	if (!FileAccess::exists(abs_path)) {
		return ai_create_error_result(AIErrorCodes::FILE_NOT_FOUND,
			vformat("File does not exist at '%s'", file_path));
	}

	// Require read_script before update_script (prevents blind overwrites)
	if (!ai_singleton->was_file_read(file_path)) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			vformat("You must call read_script or create_script on '%s' before updating it. Read the file first to understand its current contents.", file_path));
	}

	// Read existing content for undo
	Ref<FileAccess> file = FileAccess::open(abs_path, FileAccess::READ);
	if (file.is_null()) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("Failed to open file for reading at '%s'", file_path));
	}
	String original_content = file->get_as_text();
	file.unref();

	// Check if game is running (file will be locked by the running process)
	EditorRunBar *run_bar = EditorRunBar::get_singleton();
	if (run_bar && run_bar->is_playing()) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			"Cannot update script while the game is running. Stop the game (F8) first.");
	}

	// Count occurrences of old_string in file content
	int occurrence_count = 0;
	int search_from = 0;
	while (true) {
		int pos = original_content.find(old_string, search_from);
		if (pos == -1) {
			break;
		}
		occurrence_count++;
		search_from = pos + old_string.length();
	}

	if (occurrence_count == 0) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			vformat("old_string not found in '%s'. Make sure you are matching the file content exactly (including whitespace and indentation).", file_path));
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

	ai_log_verbose(vformat("Updating script at '%s'. Replaced %d occurrence(s). Old size: %d, New size: %d",
			abs_path, replace_all ? occurrence_count : 1, original_content.length(), new_content.length()));

	undo_redo->create_action("AI Update Script");
	undo_redo->add_do_method(ai_singleton, "_write_script_file", abs_path, new_content);
	undo_redo->add_undo_method(ai_singleton, "_write_script_file", abs_path, original_content);
	undo_redo->commit_action();

	// Validate written script: parser + analyzer mirrors full editor error feedback.
	Array parse_errors;
	Array warnings;
	{
		GDScriptParser parser;
		Error parse_err = parser.parse(new_content, abs_path, false);

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
	result_data["replacements"] = replace_all ? occurrence_count : 1;
	result_data["old_size"] = original_content.length();
	result_data["new_size"] = new_content.length();
	if (!parse_errors.is_empty()) {
		result_data["parse_errors"] = parse_errors;
	}
	if (!warnings.is_empty()) {
		result_data["warnings"] = warnings;
	}

	print_line(vformat("AI: Executed update_script. File: %s, %d replacement(s)", file_path, replace_all ? occurrence_count : 1));
	return ai_create_success_result(result_data);
}

Dictionary exec_attach_script(const Dictionary &args) {
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
	String script_path = args["script_path"];

	// Resolve the target node
	Node *target_node = ai_get_node_by_path(node_path_str);
	if (!target_node) {
		return ai_create_error_result(AIErrorCodes::NODE_NOT_FOUND,
			vformat("Could not find node at path '%s'", node_path_str));
	}

	// Load script resource
	Ref<Script> scr = ResourceLoader::load(script_path);
	if (scr.is_null()) {
		return ai_create_error_result(AIErrorCodes::FILE_NOT_FOUND,
			vformat("Could not load script at '%s'", script_path));
	}

	// Save old script for undo (may be null)
	Variant old_script = target_node->get("script");

	// Wrap in UndoRedo
	undo_redo->create_action("AI Attach Script");
	undo_redo->add_do_method(target_node, "set", "script", scr);
	undo_redo->add_undo_method(target_node, "set", "script", old_script);
	undo_redo->commit_action();

	Dictionary result_data;
	result_data["node_path"] = node_path_str;
	result_data["script_path"] = script_path;
	result_data["had_previous_script"] = !old_script.is_null() && old_script.get_type() != Variant::NIL;

	ai_log_verbose(vformat("attach_script to node: %s", node_path_str));
	print_line(vformat("AI: Executed attach_script. Node: %s, Script: %s", node_path_str, script_path));
	return ai_create_success_result(result_data);
}

Dictionary exec_detach_script(const Dictionary &args) {
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

	// Resolve the target node
	Node *target_node = ai_get_node_by_path(node_path_str);
	if (!target_node) {
		return ai_create_error_result(AIErrorCodes::NODE_NOT_FOUND,
			vformat("Could not find node at path '%s'", node_path_str));
	}

	// Save old script for undo (may be null)
	Variant old_script = target_node->get("script");

	Dictionary result_data;
	result_data["node_path"] = node_path_str;

	// If already detached, return success (no-op)
	if (old_script.is_null() || old_script.get_type() == Variant::NIL) {
		ai_log_verbose(vformat("Execute 'detach_script': Node '%s' already has no script attached.", node_path_str));
		result_data["was_no_op"] = true;
		return ai_create_success_result(result_data);
	}

	// Wrap in UndoRedo
	undo_redo->create_action("Detach Script");
	undo_redo->add_do_method(target_node, "set", "script", Variant());
	undo_redo->add_undo_method(target_node, "set", "script", old_script);
	undo_redo->commit_action();

	result_data["was_no_op"] = false;

	ai_log_verbose(vformat("detach_script from node: %s", node_path_str));
	print_line(vformat("AI: Executed detach_script. Node: %s", node_path_str));
	return ai_create_success_result(result_data);
}

Dictionary exec_rename_script(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	EditorUndoRedoManager *undo_redo = ai_get_undo_redo();
	if (!undo_redo) {
		return ai_create_error_result(AIErrorCodes::NO_UNDO_REDO,
			"EditorUndoRedoManager singleton not found");
	}

	AI *ai_singleton = AI::get_singleton();
	if (!ai_singleton) {
		return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			"AI singleton not found");
	}

	if (!args.has("old_path") || args["old_path"].get_type() != Variant::STRING) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"'old_path' must be a string");
	}
	if (!args.has("new_path") || args["new_path"].get_type() != Variant::STRING) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"'new_path' must be a string");
	}

	String old_path = args["old_path"];
	String new_path = args["new_path"];

	// Convert to absolute paths
	String old_abs_path = ProjectSettings::get_singleton()->globalize_path(old_path);
	String new_abs_path = ProjectSettings::get_singleton()->globalize_path(new_path);

	// Check if old file exists
	if (!FileAccess::exists(old_abs_path)) {
		return ai_create_error_result(AIErrorCodes::FILE_NOT_FOUND,
			vformat("File does not exist at '%s'", old_path));
	}

	// Check if new file already exists
	if (FileAccess::exists(new_abs_path)) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("File already exists at '%s'. Cannot rename", new_path));
	}

	ai_log_verbose(vformat("Renaming script from '%s' to '%s'", old_abs_path, new_abs_path));

	// Wrap in UndoRedo
	undo_redo->create_action("AI Rename Script");
	undo_redo->add_do_method(ai_singleton, "_rename_script_file", old_abs_path, new_abs_path);
	undo_redo->add_undo_method(ai_singleton, "_rename_script_file", new_abs_path, old_abs_path);
	undo_redo->commit_action();

	Dictionary result_data;
	result_data["old_path"] = old_path;
	result_data["new_path"] = new_path;

	print_line(vformat("AI: Executed rename_script. Old: %s, New: %s", old_path, new_path));
	return ai_create_success_result(result_data);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

namespace {

// Depth-first traversal to find and detach script from nodes.
static void ai_detach_script_from_nodes_dfs(Node *root, const String &script_path, EditorUndoRedoManager *undo_redo, Node *edited_scene_root) {
	if (!root) {
		return;
	}

	// Check if this node has the script attached
	Ref<Script> script = root->get_script();
	if (script.is_valid() && script->get_path() == script_path) {
		Variant old_script = root->get("script");
		undo_redo->add_do_method(root, "set", "script", Variant());
		undo_redo->add_undo_method(root, "set", "script", old_script);
	}

	// Recurse into children
	const int child_count = root->get_child_count();
	for (int i = 0; i < child_count; i++) {
		Node *child = root->get_child(i);
		if (child) {
			ai_detach_script_from_nodes_dfs(child, script_path, undo_redo, edited_scene_root);
		}
	}
}

} // namespace

Dictionary exec_delete_script(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	EditorUndoRedoManager *undo_redo = ai_get_undo_redo();
	if (!undo_redo) {
		return ai_create_error_result(AIErrorCodes::NO_UNDO_REDO,
			"EditorUndoRedoManager singleton not found");
	}

	AI *ai_singleton = AI::get_singleton();
	if (!ai_singleton) {
		return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			"AI singleton not found");
	}

	if (!args.has("file_path") || args["file_path"].get_type() != Variant::STRING) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"'file_path' must be a string");
	}

	String file_path = args["file_path"];
	bool detach_from_nodes = args.get("detach_from_nodes", false);

	// Convert to absolute path
	String abs_path = ProjectSettings::get_singleton()->globalize_path(file_path);

	// Check if file exists
	if (!FileAccess::exists(abs_path)) {
		return ai_create_error_result(AIErrorCodes::FILE_NOT_FOUND,
			vformat("File does not exist at '%s'", file_path));
	}

	// Read old content for undo
	Ref<FileAccess> file = FileAccess::open(abs_path, FileAccess::READ);
	if (file.is_null()) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("Failed to open file for reading at '%s'", file_path));
	}
	String original_content = file->get_as_text();
	file.unref(); // Close the file

	ai_log_verbose(vformat("Deleting script at '%s'", abs_path));

	// Wrap in UndoRedo
	undo_redo->create_action("AI Delete Script");

	// If detach_from_nodes is true, detach script from all nodes using it
	if (detach_from_nodes) {
		Node *edited_scene_root = ai_get_edited_scene_root();
		if (edited_scene_root) {
			ai_detach_script_from_nodes_dfs(edited_scene_root, file_path, undo_redo, edited_scene_root);
		}
	}

	// Delete the file
	undo_redo->add_do_method(ai_singleton, "_delete_script_file", abs_path);
	undo_redo->add_undo_method(ai_singleton, "_create_script_file", abs_path, original_content);
	undo_redo->commit_action();

	Dictionary result_data;
	result_data["file_path"] = file_path;
	result_data["detach_from_nodes"] = detach_from_nodes;

	print_line(vformat("AI: Executed delete_script. File: %s, DetachFromNodes: %s", file_path, detach_from_nodes ? "true" : "false"));
	return ai_create_success_result(result_data);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

} // namespace AIScriptActions

