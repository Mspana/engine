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

// Reads the raw text of a .tscn scene file from disk.
// Required args: file_path (String, res://... .tscn)
// Returns: content, size, is_open_in_editor, has_unsaved_changes (+ staleness warning when dirty)
Dictionary exec_read_scene_file(const Dictionary &args);

// Exact string replacement in a .tscn file on disk, with pre-commit validation
// and editor reload of the affected tab. Refuses if the scene tab has unsaved changes.
// Required args: file_path, old_string, new_string (String). Optional: replace_all (bool)
Dictionary exec_update_scene_file(const Dictionary &args);

} // namespace AISceneActions

#endif // AI_SCENE_ACTIONS_H

