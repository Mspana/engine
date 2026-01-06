// modules/ai/actions/scene_actions.h
// Scene-related action implementations for the AI module

#ifndef AI_SCENE_ACTIONS_H
#define AI_SCENE_ACTIONS_H

#include "core/variant/dictionary.h"

namespace AISceneActions {

// Opens a scene file in the editor.
// Required args: scene_path (String)
bool exec_open_scene(const Dictionary &args);

// Creates a new scene file.
// Required args: scene_path (String)
// Optional args: root_type (String, default "Node"), root_name (String, default "Main")
bool exec_create_scene(const Dictionary &args);

// Saves the current scene.
// Args: none required (optional scene_path is ignored)
bool exec_save_scene(const Dictionary &args);

// Sets the project's main scene in ProjectSettings.
// Required args: scene_path (String)
bool exec_set_main_scene(const Dictionary &args);

// Closes the current scene tab.
// Optional args: save_if_modified (bool, default true)
bool exec_close_scene(const Dictionary &args);

} // namespace AISceneActions

#endif // AI_SCENE_ACTIONS_H

