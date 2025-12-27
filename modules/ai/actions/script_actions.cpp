// modules/ai/actions/script_actions.cpp
// Script-related action implementations for the AI module

#include "script_actions.h"
#include "action_common.h"

#include "../ai.h" // For AI::get_singleton() and file helper methods

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/object/script_language.h"
#include "scene/main/node.h"

namespace AIScriptActions {

bool exec_create_script(const Dictionary &args) {
	EditorUndoRedoManager *undo_redo = ai_get_undo_redo();
	if (!undo_redo) {
		ai_log_error("Execute 'create_script': EditorUndoRedoManager singleton not found.");
		return false;
	}

	AI *ai_singleton = AI::get_singleton();
	if (!ai_singleton) {
		ai_log_error("Execute 'create_script': AI singleton not found.");
		return false;
	}

	String file_path = args["file_path"];
	String language = args["language"];
	String content = args["content"];

	// Validate language (v0: GDScript only)
	if (language != "GDScript") {
		ai_log_error(vformat("Execute 'create_script': Only 'GDScript' language is supported in v0. Got: '%s'", language));
		return false;
	}

	// Convert to absolute path if it's a resource path
	String abs_path = ProjectSettings::get_singleton()->globalize_path(file_path);

	// Check if file already exists
	if (FileAccess::exists(abs_path)) {
		WARN_PRINT(vformat("AI Execute 'create_script': File already exists at '%s'. Skipping creation.", abs_path));
		return false;
	}

	ai_log_verbose(vformat("Creating script at '%s' with %d bytes of content", abs_path, content.length()));

	undo_redo->create_action("AI Create Script");
	undo_redo->add_do_method(ai_singleton, "_create_script_file", abs_path, content);
	undo_redo->add_undo_method(ai_singleton, "_delete_script_file", abs_path);
	undo_redo->commit_action();

	print_line(vformat("AI: Executed create_script. File: %s, Language: %s", file_path, language));
	return true;
}

bool exec_update_script(const Dictionary &args) {
	EditorUndoRedoManager *undo_redo = ai_get_undo_redo();
	if (!undo_redo) {
		ai_log_error("Execute 'update_script': EditorUndoRedoManager singleton not found.");
		return false;
	}

	AI *ai_singleton = AI::get_singleton();
	if (!ai_singleton) {
		ai_log_error("Execute 'update_script': AI singleton not found.");
		return false;
	}

	String file_path = args["file_path"];
	String patch_content = args["patch"];

	// Convert to absolute path if it's a resource path
	String abs_path = ProjectSettings::get_singleton()->globalize_path(file_path);

	// Check if file exists
	if (!FileAccess::exists(abs_path)) {
		ai_log_error(vformat("Execute 'update_script': File does not exist at '%s'. Cannot update.", abs_path));
		return false;
	}

	// Read existing content for undo
	Ref<FileAccess> file = FileAccess::open(abs_path, FileAccess::READ);
	if (file.is_null()) {
		ai_log_error(vformat("Execute 'update_script': Failed to open file for reading at '%s'.", abs_path));
		return false;
	}
	String original_content = file->get_as_text();
	file.unref(); // Close the file

	ai_log_verbose(vformat("Updating script at '%s'. Old size: %d bytes, New size: %d bytes", abs_path, original_content.length(), patch_content.length()));

	undo_redo->create_action("AI Update Script");
	undo_redo->add_do_method(ai_singleton, "_write_script_file", abs_path, patch_content);
	undo_redo->add_undo_method(ai_singleton, "_write_script_file", abs_path, original_content);
	undo_redo->commit_action();

	print_line(vformat("AI: Executed update_script. File: %s", file_path));
	return true;
}

bool exec_attach_script(const Dictionary &args) {
	EditorUndoRedoManager *undo_redo = ai_get_undo_redo();
	if (!undo_redo) {
		ai_log_error("Execute 'attach_script': EditorUndoRedoManager singleton not found.");
		return false;
	}

	Node *edited_scene_root = ai_get_edited_scene_root();
	if (!edited_scene_root) {
		ai_log_error("Execute 'attach_script': No edited scene root.");
		return false;
	}

	String node_path_str = args["node_path"];
	String script_path = args["script_path"];

	// Resolve the target node
	Node *target_node = ai_get_node_by_path(node_path_str);
	if (!target_node) {
		ai_log_error(vformat("Execute 'attach_script': Could not find node at path '%s'.", node_path_str));
		return false;
	}

	// Load script resource
	Ref<Script> scr = ResourceLoader::load(script_path);
	if (scr.is_null()) {
		ai_log_error(vformat("Execute 'attach_script': Could not load script at '%s'.", script_path));
		return false;
	}

	// Save old script for undo (may be null)
	Variant old_script = target_node->get("script");

	// Wrap in UndoRedo
	undo_redo->create_action("AI Attach Script");
	undo_redo->add_do_method(target_node, "set", "script", scr);
	undo_redo->add_undo_method(target_node, "set", "script", old_script);
	undo_redo->commit_action();

	ai_log_verbose(vformat("attach_script to node: %s", node_path_str));
	print_line(vformat("AI: Executed attach_script. Node: %s, Script: %s", node_path_str, script_path));
	return true;
}

bool exec_detach_script(const Dictionary &args) {
	EditorUndoRedoManager *undo_redo = ai_get_undo_redo();
	if (!undo_redo) {
		ai_log_error("Execute 'detach_script': EditorUndoRedoManager singleton not found.");
		return false;
	}

	Node *edited_scene_root = ai_get_edited_scene_root();
	if (!edited_scene_root) {
		ai_log_error("Execute 'detach_script': No edited scene root.");
		return false;
	}

	String node_path_str = args["node_path"];

	// Resolve the target node
	Node *target_node = ai_get_node_by_path(node_path_str);
	if (!target_node) {
		ai_log_error(vformat("Execute 'detach_script': Could not find node at path '%s'.", node_path_str));
		return false;
	}

	// Save old script for undo (may be null)
	Variant old_script = target_node->get("script");

	// If already detached, return success (no-op)
	if (old_script.is_null() || old_script.get_type() == Variant::NIL) {
		ai_log_verbose(vformat("Execute 'detach_script': Node '%s' already has no script attached.", node_path_str));
		return true;
	}

	// Wrap in UndoRedo
	undo_redo->create_action("Detach Script");
	undo_redo->add_do_method(target_node, "set", "script", Variant());
	undo_redo->add_undo_method(target_node, "set", "script", old_script);
	undo_redo->commit_action();

	ai_log_verbose(vformat("detach_script from node: %s", node_path_str));
	print_line(vformat("AI: Executed detach_script. Node: %s", node_path_str));
	return true;
}

} // namespace AIScriptActions

