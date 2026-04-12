// modules/ai/actions/project_actions.h
// Project settings-related action implementations for the AI module

#ifndef AI_PROJECT_ACTIONS_H
#define AI_PROJECT_ACTIONS_H

#include "core/variant/dictionary.h"

namespace AIProjectActions {

// Sets a project setting value.
// Required args: key (String), value (Variant)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_set_project_setting(const Dictionary &args);

// Gets project settings (read-only).
// Optional args: prefix (String), keys (Array[String]), include_defaults (bool, default false)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_get_project_settings(const Dictionary &args);

// Creates an autoload singleton entry.
// Required args: name (String), script_path (String)
// Optional args: enabled (bool, default true)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_create_autoload_singleton(const Dictionary &args);

// Removes an autoload singleton entry.
// Required args: name (String)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_remove_autoload_singleton(const Dictionary &args);

// Imports an asset file from OS path to project path.
// Required args: source_path (String, absolute OS path), dest_path (String, res://...)
// Optional args: overwrite (bool, default false)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_import_asset(const Dictionary &args);

// Deletes an asset file from the project.
// Required args: asset_path (String, res://...)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_delete_asset(const Dictionary &args);

// Runs/plays the project.
// Optional args: mode (String, default "play"), scene_path (String)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_run_project(const Dictionary &args);

// Runs the project, waits, captures a screenshot, then stops the game.
// Optional args: wait_seconds (float, default 2.0, clamped 0.5-10.0)
// Returns: Dictionary with status="success"|"error", result: { screenshot (base64 PNG), wait_seconds }
Dictionary exec_run_and_screenshot(const Dictionary &args);

// Captures the 2D editor viewport as a base64 PNG and attaches it via _images
// so the orchestrator forwards it to vision-capable models.
// No args.
// Returns: Dictionary with status="success"|"error", result: { width, height, screenshot_b64 }
Dictionary exec_capture_2d_viewport(const Dictionary &args);

// Captures the currently active 3D editor viewport as a base64 PNG.
// No args.
// Returns: Dictionary with status="success"|"error", result: { width, height, screenshot_b64 }
Dictionary exec_capture_3d_viewport(const Dictionary &args);

} // namespace AIProjectActions

#endif // AI_PROJECT_ACTIONS_H

