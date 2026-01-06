// modules/ai/actions/scene_actions.cpp
// Scene-related action implementations for the AI module

#include "scene_actions.h"
#include "action_common.h"

#include "editor/editor_interface.h"
#include "editor/editor_command_palette.h"
#include "core/string/ustring.h"
#include "core/object/class_db.h"
#include "scene/resources/packed_scene.h"
#include "core/io/resource_saver.h"
#include "scene/main/node.h"
#include "core/config/project_settings.h"
#include "core/io/resource_loader.h"

namespace AISceneActions {

bool exec_open_scene(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		ai_log_error("Execute 'open_scene': EditorInterface singleton not found.");
		return false;
	}

	String scene_path = args["scene_path"];
	if (scene_path.is_empty()) {
		ai_log_error("Execute 'open_scene': scene_path is empty.");
		return false;
	}

	ei->open_scene_from_path(scene_path);
	print_line(vformat("AI: Executed open_scene. Path: %s", scene_path));
	return true;
#else
	ai_log_error("Execute 'open_scene': Editor API not available in non-editor builds.");
	return false;
#endif
}

bool exec_create_scene(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		ai_log_error("Execute 'create_scene': EditorInterface singleton not found.");
		return false;
	}

	String scene_path = args["scene_path"];
	if (scene_path.is_empty()) {
		ai_log_error("Execute 'create_scene': scene_path is empty.");
		return false;
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
		ai_log_error(vformat("Execute 'create_scene': Failed to instantiate node of type '%s'.", root_type));
		return false;
	}

	root_node->set_name(root_name);

	// Pack into PackedScene
	Ref<PackedScene> packed_scene;
	packed_scene.instantiate();
	packed_scene->pack(root_node);

	// Save to scene_path
	Error err = ResourceSaver::save(packed_scene, scene_path);
	if (err != OK) {
		ai_log_error(vformat("Execute 'create_scene': Failed to save scene to '%s'. Error: %d", scene_path, err));
		root_node->queue_free();
		return false;
	}

	// Open the scene in the editor
	ei->open_scene_from_path(scene_path);

	print_line(vformat("AI: Executed create_scene. Path: %s, RootType: %s, RootName: %s", scene_path, root_type, root_name));
	return true;
#else
	ai_log_error("Execute 'create_scene': Editor API not available in non-editor builds.");
	return false;
#endif
}

bool exec_save_scene(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		ai_log_error("Execute 'save_scene': EditorInterface singleton not found.");
		return false;
	}

	EditorCommandPalette *command_palette = ei->get_command_palette();
	if (!command_palette) {
		ai_log_error("Execute 'save_scene': EditorCommandPalette not found.");
		return false;
	}

	command_palette->execute_command("editor/save_scene");
	print_line("AI: Executed save_scene.");
	return true;
#else
	ai_log_error("Execute 'save_scene': Editor API not available in non-editor builds.");
	return false;
#endif
}

bool exec_set_main_scene(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	String scene_path = args.get("scene_path", String());
	if (scene_path.is_empty()) {
		ai_log_error("Execute 'set_main_scene': scene_path is empty.");
		return false;
	}

	// Validate that the scene resource exists.
	if (!ResourceLoader::exists(scene_path)) {
		ai_log_error(vformat("Execute 'set_main_scene': Scene file does not exist at path '%s'.", scene_path));
		return false;
	}

	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		ai_log_error("Execute 'set_main_scene': ProjectSettings singleton not available.");
		return false;
	}

	const String setting_key = "application/run/main_scene";
	ps->set_setting(setting_key, scene_path);

	Error err = ps->save();
	if (err != OK) {
		ai_log_error(vformat("Execute 'set_main_scene': Failed to save ProjectSettings (project.godot). Error: %d", err));
		return false;
	}

	print_line(vformat("AI: Executed set_main_scene. Main scene set to: %s", scene_path));
	return true;
#else
	ai_log_error("Execute 'set_main_scene': Editor API not available in non-editor builds.");
	return false;
#endif
}

} // namespace AISceneActions

