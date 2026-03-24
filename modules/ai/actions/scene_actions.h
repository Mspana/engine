// modules/ai/actions/scene_actions.h
// Scene-related action implementations for the AI module

#ifndef AI_SCENE_ACTIONS_H
#define AI_SCENE_ACTIONS_H

#include "core/variant/dictionary.h"

namespace AISceneActions {

// Opens a scene file in the editor.
// Required args: scene_path (String)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_open_scene(const Dictionary &args);

// Creates a new scene file.
// Required args: scene_path (String)
// Optional args: root_type (String, default "Node"), root_name (String, default "Main")
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_create_scene(const Dictionary &args);

// Saves the current scene.
// Args: none required (optional scene_path is ignored)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_save_scene(const Dictionary &args);

// Sets the project's main scene in ProjectSettings.
// Required args: scene_path (String)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_set_main_scene(const Dictionary &args);

// Closes the current scene tab.
// Optional args: save_if_modified (bool, default true)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_close_scene(const Dictionary &args);

// Lists all currently open scene tabs in the editor.
// Args: none
// Returns: scenes array with path + is_current, count, current_scene
Dictionary exec_list_open_scenes(const Dictionary &args);

// Stops the running game. No-op if game is not running.
// Args: none
// Returns: Dictionary with was_playing bool
Dictionary exec_stop_game(const Dictionary &args);

} // namespace AISceneActions

#endif // AI_SCENE_ACTIONS_H

