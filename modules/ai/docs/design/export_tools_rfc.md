# RFC: Export Tools for the AI Agent

## 1. Objective

Give the in-engine AI agent the ability to take a project from "done" to "exported and verified" — create export presets, check export readiness, run the export, and locally test web builds — without the user having to walk the export wizard themselves. Hosting/deployment (itch.io, GitHub Pages, etc.) stays explicitly out of scope: the agent's job ends at a verified build on disk.

## 2. Why

Observed in a real session (chat `1784782538812`, July 2026): the user asked "time to export, I'd like this to be a webpage, what are my options?" The agent handled the parts it had tools for (diagnosed the Forward+ renderer as web-incompatible, switched to `gl_compatibility`, checked the shaders folder) and then had to say:

> "I can't install export templates or create the export preset — those are editor UI steps on your side."

That claim turned out to be wrong about the engine but right about the agent: every one of those steps has a callable C++ API inside the editor process. The agent just doesn't have tools wrapping them. Export is also squarely an "editor-semantic" capability per `in_engine_vs_external_agents.md` — it needs the live `EditorExport` singleton, the platform registry, and (for web) the editor's own dev server, none of which an external agent editing files can reach.

The single biggest win is web-specific: Godot 4 web builds require cross-origin isolation headers (COOP/COEP) and won't boot from a plain file open or naive local server. The editor ships a local HTTP server with those headers **hard-coded** — so the agent could export and hand the user a working `localhost` URL to verify the build in a real browser, closing the loop that hosting-header confusion usually breaks.

## 3. What the Engine Already Provides

All of this is callable from module C++ in the editor process. No engine changes are strictly required for Tiers 1–2 below.

### Presets

- Presets live in `res://export_presets.cfg`, written by `EditorExport::_save()` (`editor/export/editor_export.cpp:39`). Saving is debounced through a 0.8 s timer (`save_presets()`, `:113`); mutating a preset value triggers it automatically.
- The correct way to create a preset is **from the platform**: `EditorExportPlatform::create_preset()` (`editor/export/editor_export_platform.cpp:427`) instantiates it with every option populated with defaults. Register it via `EditorExport::add_export_preset()` (`editor_export.cpp:154`).
- Platforms are found by iterating `EditorExport::get_export_platform_count()` / `get_export_platform(i)` and matching `get_name()` (the web platform returns `"Web"`).
- A default web preset is valid as-is — the caller only needs to set a name, an `.html` export path, and `runnable = true` (required for the one-click run flow). Web option defaults: `variant/thread_support = false`, `vram_texture_compression/for_desktop = true`, PWA off.

### Readiness / validation

- `EditorExportPlatform::can_export()` (`editor_export_platform.cpp:2276`) aggregates template presence, plugin warnings, and project-configuration checks into human-readable error strings — exactly what the agent should surface when diagnosing "why can't I export yet."
- `can_export()` is necessary but not sufficient — it validates the platform, not the preset's content. Observed 2026-07-23: a preset in "selected resources" mode (`export_filter="resources"`) with an empty `export_files` list exports with zero errors, but the pck contains only the autoloads and their preload chains (43 KB instead of 8 MB for the test project). Related trap: `include_filter` is a glob list for non-resource files, so pasting `res://` paths into it silently matches nothing. Nothing in the engine warns about either state — upstream arguably owes users a dialog here.
- Per-file template check: `exists_export_template()` (`editor_export_platform.cpp:423`). Templates live in `<editor data dir>/export_templates/<GODOT_VERSION_FULL_CONFIG>/`.
- Web export is blocked entirely in C#/Mono builds (`platform/web/export/export_plugin.cpp:405`).

### Running an export

- `EditorExportPlatform::export_project(preset, debug, path, flags)` (`editor/export/editor_export_platform.h:337`) is **synchronous and main-thread**. There is no async variant; the headless CLI path (`--export-release`) runs the same call inline and then exits the editor, so an in-process agent should call `export_project()` directly.
- Error reporting is message-based: call `clear_messages()` before, then read `get_message_count()` / `get_message(i)` / `get_worst_message_type()` after. These are categorized, human-readable, and ideal to return verbatim in a tool result.
- There is no percentage-progress signal for external listeners. Realistic progress reporting is: the final `Error`, plus the message log. (`EditorProgress` drives the modal UI during packing and is UI-coupled.)

### Local web testing (the gem)

- `EditorExportPlatformWeb::run(preset, option, flags)` (`platform/web/export/export_plugin.cpp:765`) implements the editor's one-click "Run in Browser": exports to `<temp dir>/web/tmp_js_export.html`, starts the server, opens the browser. Option `1` does export + serve **without** opening a browser.
- The server, `EditorHTTPServer` (`platform/web/export/editor_http_server.cpp`), hard-codes the critical headers — `Cross-Origin-Opener-Policy: same-origin`, `Cross-Origin-Embedder-Policy: require-corp` (`:107`) — plus `Cache-Control: no-store`. It runs its own poll thread; `listen()` / `stop()` / `is_listening()` are public.
- Constraint: the server only serves from the fixed `<temp dir>/web` directory, so "serve an arbitrary build" really means "drive the platform's own export-to-temp flow," which is fine for verification purposes.
- Host/port come from editor settings `export/web/http_host` / `http_port` (default `localhost:8060`).

### Templates (the awkward one)

- Web template download/install exists **only** inside the `ExportTemplateManager` dialog (`editor/export/export_template_manager.cpp`) as private, UI-coupled methods: mirrorlist fetch → `HTTPRequest` download → `_install_file_selected()` (zip extraction, `:427`). Android — unlike Web — has clean public install APIs.
- Practical options for an agent: (a) pop the dialog for the user via `popup_manager()`; (b) download the `.tpz` independently and replicate the ~150 self-contained lines of extraction into the templates dir. The official `.tpz` bundles all platforms and is a multi-hundred-MB download, so a real installer tool needs background download with progress and cancellation.
- Manual fallback that already exists: `misc/scripts/install_export_templates.py` downloads the era-matched upstream snapshot (`4.5-dev3` for the current fork base) and installs it under this build's version string. An `install_export_templates` tool should reuse its version-mapping logic.

## 4. Proposed Tools

Ordered by value-to-effort. Tier 1 fits the existing synchronous `exec_*` pattern directly; nothing in it is long-running.

### Tier 1 — read and configure (synchronous)

| Tool | What it does |
|---|---|
| `get_export_status` | Read-only. Lists available platforms, existing presets and their key options, whether templates for the current engine version are installed, and the `can_export()` verdict with its error strings per preset. Must also flag the silent misconfigurations `can_export()` misses: resources-mode presets with an empty `export_files` list, and `include_filter` entries that match zero files (both observed producing a near-empty pck with no engine warning). |
| `create_export_preset` | Creates a preset for a named platform via `create_preset()`, sets name / export path / runnable, registers it. Returns the resulting option set. |
| `set_export_preset_option` | Narrow key/value setter on an existing preset (mirrors the `set_project_setting` convention). Covers thread support, VRAM compression, PWA, custom HTML shell, etc. |

With just Tier 1, the agent in the motivating chat could have created the Web preset and told the user precisely what remained (install templates via the dialog), instead of describing wizard steps from general knowledge.

### Tier 2 — export and verify

| Tool | What it does |
|---|---|
| `export_project` | Pre-flights with `can_export()` plus the preset-content checks above, runs `export_project()`, returns the message log plus a listing of output files and sizes — including the pck's embedded file count, which is the tripwire for silent near-empty exports (a clean log proved meaningless in the 2026-07-23 case). Optionally zips the output (itch.io-ready — still local file work, not hosting). |
| `serve_web_build` | Drives `EditorExportPlatformWeb::run()`: export to temp, start the header-correct local server, optionally open the browser. Returns the URL. A companion stop (or an option on the same tool) shuts the server down. |

For a small 2D project, `export_project` completes in seconds, so it can start life as a normal synchronous tool. If large projects make blocking unacceptable, the module already has the async precedent to copy: the `run_and_screenshot` state machine in `agentic_orchestrator.cpp` (SceneTreeTimer polling, manual tool-result append, loop resume).

### Tier 3 — templates

| Tool | What it does |
|---|---|
| `open_template_manager` | Cheap stopgap: pops the Manage Export Templates dialog so the user clicks Download once. |
| `install_export_templates` | Full version: fetch the mirrorlist, download the `.tpz` in the background (precedent: the provider's `WorkerThreadPool` + `HTTPClient` pattern in `ai_provider.cpp`), extract into the templates dir, report progress. |

Recommendation: ship `open_template_manager` with Tier 1 and defer the real installer until the rest proves out. Template install is a once-per-engine-version action; the dialog is genuinely fine.

## 5. Module Integration

Standard three-touch tool addition, no new architecture for Tiers 1–2:

1. Schema blocks in `modules/ai/tools_array.inc`.
2. Dispatch branches in `AI::execute_single_action()` (`modules/ai/ai.cpp:582`).
3. New `modules/ai/actions/export_actions.{h,cpp}` following the `project_actions` pattern, returning through `ai_create_success_result` / `ai_create_error_result`.

There is currently no export, build, or network-touching tool in the module — this is greenfield. It would also be the first "manage a named config collection" tool (presets), so the schema shapes set convention there.

## 6. Explicitly Out of Scope

- **Hosting and deployment.** No uploading to itch.io, no GitHub Pages, no git operations, no external publishing of any kind. The agent's deliverable is a verified build (and optionally a zip) on local disk; where it goes after that is the user's call.
- **Non-web platform depth.** The APIs above are platform-generic and the tools should be too, but web is the design driver; platform-specific concerns like Android keystores or macOS signing are not addressed here.

## 7. Open Questions (resolved at implementation — see `../architecture/export_tools.md`)

- ~~Should `export_project` and `serve_web_build` be one tool?~~ **Separate tools.** "Give me build files at this path" and "let me try it in the browser" are distinct intents; the serve flow also always exports a *debug* build to the editor temp dir, which would muddy a combined tool's contract.
- ~~Preset mutation and undo~~ — **No undo**, consistent with `set_project_setting`; every mutating result notes that `export_presets.cfg` was rewritten.
- ~~Debounced preset save~~ — **Accepted.** A crash within ~0.8 s of a preset mutation loses it; preset creation orders its calls so a setter arms the save after registration.
- Implementation extras beyond this RFC: the empty-file-list case is a hard *block* in `export_project`/`serve_web_build` (not just a status warning); zip output for itch.io was deferred (the result's file listing tells the user what to zip); the template installer is fully native (threaded HTTPRequest download + WorkerThreadPool extraction) rather than a dialog hand-off.
