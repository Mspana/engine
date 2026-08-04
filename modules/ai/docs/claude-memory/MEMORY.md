# Project Memory

## Don't commit unless asked
User preference: never commit unless explicitly requested.

## Future Optimization: Eliminate Double GDScript Compilation in update_script
`update_script` currently runs `GDScriptParser + GDScriptAnalyzer` once synchronously
(to populate `parse_errors` in the tool result) and then `scan_changes()` causes the
editor to recompile again asynchronously. The double run is intentional and accepted
(each run serves a different purpose). To eliminate it: call `gd_script->reload()`
synchronously in `_write_script_file()` and expose post-reload errors from the script
object, removing the need for our own parse+analyze pass. Requires a clean error API
on `GDScript` or refactoring so validation and write happen in the same call.

## Feature Testing Feedback
- [Feature testing feedback](feedback_feature_testing.md) — screenshot fix rejected (kills game), I/O button removed, property monitor needs full redesign as "monitored play"

## Builds
- [Builds feedback](feedback_builds.md) — user runs scons builds themselves; don't launch compiles unprompted

## Scene File Editing
- [Scene file editing autosave direction](scene-file-editing-autosave-direction.md) — update_scene_file refuses on dirty tabs; future = Google-Docs-style autosave, not smarter refusals

## AI Journal Logs
Run records and dev notes are written to `user://ai_journal/` → `C:\Users\Matthew\AppData\Roaming\Godot\app_userdata\test project\ai_journal\`. Files: `runs.jsonl` (full run records) and `dev_notes.jsonl` (agent-authored notes). Logic in `modules/ai/ai.cpp:_on_agentic_complete`.

## AI Chat Log Locations
- [AI chat log locations](ai-chat-log-locations.md) — newest chats live in AppData `app_userdata\<project>\ai_chat`, Documents folder is junctions; active test project as of 7/2026 is "a personal vibe"
- [gibdulbas.it Pages deploy](gibdulbasit-pages-deploy.md) — repo location, headless export command, empty-pck preset gotcha

## Cursor Workspace Setup
- [Cursor workspace setup](cursor-workspace-setup.md) — engine opens via multi-root engine.code-workspace; window-level settings go in that file, not folder .vscode/settings.json

## Harness UI Parity
- [Harness UI parity target](harness-ui-parity-target.md) - modern agent UI beats legacy dev-feature parity; drop token pills etc. where codex lacks equivalents
