// modules/ai/actions/export_actions.h
// Export-related action implementations for the AI module

#ifndef AI_EXPORT_ACTIONS_H
#define AI_EXPORT_ACTIONS_H

#include "core/variant/dictionary.h"

// Era-matched upstream export-template package for this fork.
// Keep in sync with misc/scripts/install_export_templates.py (fork→upstream
// mapping rationale lives there): the fork branched from upstream 4e6451d62a
// (2025-04-25), the same day the 4.5-dev3 snapshot was published.
#define AI_EXPORT_TEMPLATES_RELEASE_TAG "4.5-dev3"
#define AI_EXPORT_TEMPLATES_URL "https://github.com/godotengine/godot-builds/releases/download/4.5-dev3/Godot_v4.5-dev3_export_templates.tpz"

namespace AIExportActions {

// Read-only overview of export readiness: platforms, presets, template
// installation state, per-preset can-export verdicts, and misconfiguration
// warnings that the engine itself does not surface.
// Optional args: platform (String, filter presets/platforms by name)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_get_export_status(const Dictionary &args);

// Creates an export preset for a named platform with all option defaults.
// Required args: platform (String, e.g. "Web")
// Optional args: name (String, default = platform name), export_path (String),
//                runnable (bool, default true), export_filter (String enum)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_create_export_preset(const Dictionary &args);

// Sets one option on an existing export preset. Handles structural keys
// (name, export_path, runnable, export_filter, include_filter, exclude_filter,
// export_files) via typed setters and validates platform option names against
// the preset's property list (the engine itself stores unknown keys silently).
// Required args: preset (String), option (String), value (Variant)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_set_export_preset_option(const Dictionary &args);

// Exports the project using an existing preset. Pre-flights can_export() plus
// content checks, then verifies the output (file listing, pck embedded file
// count read from the pck header).
// Required args: preset (String)
// Optional args: output_path (String, default = preset's export_path),
//                debug (bool, default false)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_export_project(const Dictionary &args);

// Exports a temp DEBUG web build and serves it on the editor's local HTTP
// server (which hard-codes the COOP/COEP headers web builds require).
// Optional args: action (String: "serve"|"serve_and_open"|"stop",
//                default "serve_and_open"), preset (String)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_serve_web_build(const Dictionary &args);

// Opens the Manage Export Templates dialog.
// No args.
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_open_template_manager(const Dictionary &args);

// Template install: synchronous fast paths only (already-installed check).
// The actual download+extract is driven asynchronously by the orchestrator
// (see agentic_orchestrator.cpp) — this function is only reached directly
// when templates are already installed or the tool is invoked outside the
// agent loop.
// Optional args: force (bool, reinstall over an existing dir)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_install_export_templates(const Dictionary &args);

// === Helpers shared with the orchestrator's async template installer ===

// Absolute path of this engine version's template folder
// (<data dir>/export_templates/<GODOT_VERSION_FULL_CONFIG>).
String get_templates_dir();

// True if the template folder exists and is non-empty.
bool templates_installed();

// Number of files in the template folder (0 if absent).
int count_template_files();

} // namespace AIExportActions

#endif // AI_EXPORT_ACTIONS_H
