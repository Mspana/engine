# Export Tools

The AI agent can take a project from "done" to "exported and verified": inspect export readiness, create and configure export presets, run exports, locally serve web builds for testing, and install export templates. Hosting/deployment (itch.io, GitHub Pages, etc.) is explicitly out of scope — the agent's deliverable ends at a verified build on disk. Design rationale lives in `../design/export_tools_rfc.md`.

## Tool inventory and intended workflow

| Tool | Role |
|---|---|
| `get_export_status` | Read-only. Platforms, presets with can-export verdicts, template installation, renderer info, misconfiguration warnings. The intended starting point. |
| `create_export_preset` | Create a preset for a platform with all option defaults (the engine fills them in via `create_preset()`). |
| `set_export_preset_option` | One key/value change on a preset — structural keys (name, path, filters, file list) or platform options. |
| `export_project` | Run the export synchronously, return the message log and output verification. |
| `serve_web_build` | Export a temp debug web build and serve it on the editor's local HTTP server; returns the localhost URL. |
| `install_export_templates` | Background download + install of the era-matched template package (async, with progress and cancel). |
| `open_template_manager` | Pops the Manage Export Templates dialog (inspection/uninstall only on this fork). |

Typical flow: status → create preset → (install templates once) → export → serve. The tool descriptions teach this sequence, so the model self-navigates it.

Preset mutations rewrite `res://export_presets.cfg` through the engine's own debounced save (~0.8 s) and are not undoable — same policy as `set_project_setting`. Tool results say so.

## The silent-misconfiguration tripwires

`can_export()` validates the platform, not the preset's content. Two real-world traps produce a near-empty build with zero engine errors (observed 2026-07-23: a 43 KB pck instead of 8 MB):

1. **Selective filter with an empty file list** — `export_filter` of `scenes`/`resources` with no `export_files` exports only autoloads and their preload chains.
2. **Dead filter globs** — `include_filter`/`exclude_filter` entries are comma-separated case-insensitive globs matched against both `res://dir/file` and `dir/file`; a pasted `res://` path with no wildcard matches nothing, silently.

Policy: case 1 is a **hard block** in `export_project` and `serve_web_build` (error `empty_export_file_list`; changing the filter is the override) and a warning in `get_export_status`. Case 2 is a warning everywhere, never a block (a glob can legitimately predate the files it will match).

As a post-export ground truth, `export_project` reads the pck header directly (file count is a u32 at byte offset 96 of the v2 pck format) and warns when the embedded file count is suspiciously low. A clean export log proved meaningless in the motivating incident; the pck count is the tripwire that can't lie.

## Web serving mechanics

`serve_web_build` drives the same one-click flow as the editor's Remote Deploy button: the Web export platform exports a **debug** build to the editor temp dir and serves it on the built-in `EditorHTTPServer`, which hard-codes the `Cross-Origin-Opener-Policy` / `Cross-Origin-Embedder-Policy` headers Godot web builds require — the reason opening the exported `.html` directly, or via a naive local server, will not boot.

Implementation notes:
- The platform's remote-debug state is private, but readable through `get_options_count()` after `poll_export()`: 0 = unavailable, 2 = available, 3 = serving. `run()` hard-fails unless `poll_export()` ran first, and `poll_export()` keys off the *first runnable* Web preset — so the tool requires a runnable preset and pre-checks `can_export(debug=true)` (serving needs the debug templates).
- Stop is `run(preset, option 2)` while serving; `poll_export()` itself stops an orphaned server when no runnable preset remains, which is what makes preset-less "stop" work.
- The served build lives in the editor temp dir and is separate from `export_project` output. The URL comes from the `export/web/http_host`/`http_port` editor settings (default `localhost:8060`).

## The async template installer

`install_export_templates` is the module's second async tool, a deliberate copy of the `run_and_screenshot` pattern (see `screenshot_capture.md`): intercepted by name in the orchestrator's dispatch loop before normal execution, driven by a generation-guarded `SceneTreeTimer` tick, finished by manually appending the tool-result message and resuming the model loop. Like `run_and_screenshot`, it short-circuits its batch — the schema tells the model to call it alone.

Phases:
1. **Downloading** — a threaded `HTTPRequest` node (parented under `EditorNode`; the orchestrator is `RefCounted`, not a node) streams the `.tpz` to the editor temp dir. GitHub's redirect is handled by `HTTPRequest` itself. The tick emits progress ("Downloading export templates: 213 / 850 MB") and runs a 60-second stall watchdog.
2. **Extracting** — a `WorkerThreadPool` task unzips into the templates directory (a ~1 GB main-thread unzip would freeze the editor for many seconds). Progress flows through atomic counters in a heap context that is refcounted by both sides, so cancel never blocks on the worker: the last owner frees it.

Cancel (the run's normal cancel path) aborts the download or flags the extraction task, cleans up the temp file, and lets the existing synthesized-result machinery answer the tool call.

If templates are already installed (and `force` is not set), the interception declines and the tool runs synchronously, returning an already-installed result instantly.

## Why export_project and serve_web_build re-enter via a timer

Tool dispatch runs from `call_deferred`, i.e. during a message-queue flush — and `export_project()` internally drives `EditorProgress`, whose dialog refuses to start in that context (`ProgressDialog::add_task` errors, then every `task_step` spams the log for the whole export). Both export-flavored tools are therefore intercepted in the dispatch loop and re-entered from a one-shot `SceneTreeTimer` callback, outside the flush; the tool itself stays synchronous, and the user gets the normal export progress bar. Any other calls batched after them are answered with a synthetic `skipped` result (the model re-issues them next turn) so no tool call is left orphaned.

## The fork → upstream template mapping

Official template packages only exist for upstream builds. This fork installs the era-matched upstream snapshot (`4.5-dev3`, same-day as the fork's branch point) under the fork's own version folder (`4.5.dev`), deliberately ignoring the package's internal `version.txt`. The URL constants live in `actions/export_actions.h` with a cross-reference to `misc/scripts/install_export_templates.py`, which documents the mapping rationale and must be kept in sync if the fork is rebased. This is also why `open_template_manager`'s online download is useless here — the dialog looks up a mirrorlist for `4.5.dev`, which has no era-matched package.

## Code map

- `actions/export_actions.{h,cpp}` — all seven tool implementations + shared helpers (preset lookup, misconfiguration checks, pck header reader, templates dir).
- `agentic_orchestrator.{h,cpp}` — the async installer state machine (`_async_tpl_*`, `_install_templates_tick`, `_on_async_tpl_complete`).
- `tools_array.inc` — the "Export actions" schema section.
- `ai.cpp` — dispatch branches.
