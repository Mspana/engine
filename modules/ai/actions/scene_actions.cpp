// modules/ai/actions/scene_actions.cpp
// Scene-related action implementations for the AI module

#include "scene_actions.h"
#include "action_common.h"

#include "editor/editor_interface.h"
#include "editor/editor_command_palette.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/gui/editor_run_bar.h"
#include "core/string/ustring.h"
#include "core/object/class_db.h"
#include "scene/resources/packed_scene.h"
#include "core/io/resource_saver.h"
#include "scene/main/node.h"
#include "core/config/project_settings.h"
#include "core/io/resource_loader.h"

namespace AISceneActions {

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

