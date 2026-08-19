# Script Execution — Architecture Plan

*August 2026, to be explored on a dedicated branch. Successor to the per-feature tool
model in [harness_replacement_plan.md](harness_replacement_plan.md): replace ~55
hand-built editor tools with one primitive — the agent writes GDScript, the editor
runs it in-process against the live engine API.*

---

## 1. Why

A tool-per-feature agent is limited to the tools that were created. Script execution
against the engine API is limited only by what the engine can do. That is the entire
argument: **this is the simplest way to have the maximum amount of functionality** —
everything we were trying to give the agent, without building everything by hand.

Evidence (chat `chat_1786686720273`, the 10-minute-level test): the agent was asked to
paint a tilemap. No TileMap tool existed. It fell back through every sanctioned path —
hand-authored `.tscn` text, reverse-engineered the 4.5 binary `tile_map_data` format
from engine source, generated correct data with Python — and still failed, because
every byte had to flow through the model's token stream. With script execution the
same task is ~15 lines of GDScript (`create_tile`, `set_cell`, `ResourceSaver`), the
engine serializes its own data, and the format knowledge problem disappears. The next
uncovered feature (terrains, nav meshes, animation state machines) requires no new
tool.

The sandbox boundary becomes the engine API itself: the agent can do anything Godot
can do and nothing else. No filesystem free-roam, targeted at game development by
construction.

## 2. The four parts

| # | Part | What it is | Status |
|---|------|------------|--------|
| 1 | Backend | Codex harness (app-server, translator, kimi-k3 et al.) | Done — unchanged |
| 2 | UI | Chat panel, checkpoints, message editing, compaction, multi-chat | Unchanged by this work |
| 3 | Acting | **One script-execution tool against the in-process engine API** | **The big change — build first** |
| 4 | Feedback | Script results, static error deltas, runtime error log, visual capture | Phased (§5) |

## 3. The loop

```
              ┌── every turn: NEW static errors (delta, auto-pushed) ──┐
              │                                                        │
   ┌──────────┴─────┐   script text    ┌────────────────────────────────────┐
   │                │ ───────────────▶ │ run_editor_script                  │
   │     Model      │                  │   (in the editor process)          │
   │ (Codex harness)│ ◀─────────────── │ checkpoint → compile from string   │
   │                │   result:        │ → run against live engine API      │
   │                │   return value,  │   (read AND write, same scene      │
   │                │   prints, errors │    the user is looking at)         │
   │                │                  └──────┬──────────────────┬──────────┘
   │                │                         │                  │
   │                │                         ▼                  ▼
   │                │                  live editor state   codex rollout
   │                │                  (instant, no        (full script + result
   │                │                   reload dance)       persisted — for
   │                │                                       developers, not
   │                │                                       the model)
   │                │
   │                │   run the game ──▶ game process ──▶ runtime error log
   │                │ ◀── run digest (counts, crash?, log path) ──┘  (JSONL)
   │                │
   │                │ ── pull on demand: read log / read-only script /
   └────────────────┘    viewport capture / run_and_screenshot ──▶
```

## 4. Part 3 — `run_editor_script` (the build focus)

**Tool contract (v1, deliberately minimal):**

- Input: `script` (full GDScript source). No timeout parameter in v1 — real
  enforcement needs off-main-thread execution (backlog, §8).
- The source starts with `@tool` and defines `func run() -> Variant`, top-level;
  implicit base is RefCounted unless the source declares `extends`. `@tool` is REQUIRED
  and enforced with a clear rejection — the editor process runs only tool scripts
  (`ScriptServer::set_scripting_enabled(false)`; a non-tool script would silently get a
  placeholder instance), and requiring the annotation rather than injecting it keeps
  error line numbers 1:1 with the source the model sent.
- Output — synchronous, always, in the tool result:
  - Success: `return_value` (JSON-serialized), captured `prints`, any engine errors
    emitted during execution, execution time.
  - Failure: parse errors **with line numbers**, or runtime exception with stack.
  - The model never fishes in a log for the outcome of its own action.

**Execution model:**

- Compiled from string in memory (`GDScript.new()` → `source_code` → instantiate).
  Never written to `res://`, never imported, invisible to the FileSystem dock.
- Runs on the editor main thread, in-process — same scene tree the user sees.
  Changes are live instantly. No disk round-trip, no "file changed on disk", no
  reload. (Of the current tools, only `update_scene_file` has the reload dance —
  because it is the only one that goes through disk.)
- Reads are first-class: `get_property_list()`, tree walking, `EditorInterface.
  get_selection()` (what the user has selected), and `ClassDB` reflection — the
  agent can ask the engine what properties a class has instead of guessing.

**Rails (v1):**

- Auto-checkpoint before every run (existing checkpoint system).
- Error-capture wrapper around execution (existing pattern from `update_scene_file`).
- Hang: GDScript on the main thread cannot be preempted — a runaway loop freezes the
  editor. v1 mitigations are checkpoint-before-run plus restart-and-restore recovery
  (chat store already survives restarts). Managed, not eliminated.
- Undo: scripts that mutate directly bypass `UndoRedo`. Same trade `update_scene_file`
  already made — checkpoint is the rollback. Script-side undo helper is future work.

**Persistence:** no dedicated script journal. Codex rollouts
(`codex_home/sessions/.../rollout-*.jsonl`) already record every dynamic tool call
with full arguments and full output, untruncated (verified 8/14 against a live
session). Scripts and their results are therefore inspectable there for free; the
chat store keeps its copy until the duplicate-transcript cleanup
([thoughts.md](../backlog/thoughts.md)) lands.

**Prompt:** the current tool descriptions become a cookbook of reference scripts
(create a node, set a property, build a TileSet, paint cells…). Examples, not a
contract.

## 5. Part 4 — feedback, phased

Organizing rule: **push consequences, push deltas of ambient state, pull bulk
history.**

- **Phase A (ships with the tool):** the synchronous result contract above. This is
  the tight loop; its quality is the agent's iteration speed.
- **Phase B (static errors) — BUILT (Aug 2026):** keep the existing delta push — new-since-last-turn
  errors each turn, not full dumps. First fix: `update_scene_file` captures load
  errors but discards them on the success path (`scene_actions.cpp` validation
  block) — captured errors must always flow to the model. The tilemap chat shipped a
  broken file as a clean success because of this.
  *Built (verified live 8/18 — a corrupt Curve sub_resource surfaced `load_errors` on a
  success result):* `update_scene_file` now returns captured non-fatal errors as `load_errors`
  (plus a warning) on the success path; `run_editor_script`'s two remaining
  discard paths (missing `run()`, base-type instantiation failure) now carry captured
  errors in their error details. All other capture/validation sites audited clean —
  script tools and `read_script` already attach `parse_errors`/`warnings` to success
  results. Delta push verified: debugger errors clear on game launch, dedupe by
  message+location with occurrence counts, and the `_errors_consumed_by_tool` flag
  prevents tool-result/injection double delivery.
- **Phase C (runtime error log) — BUILT (Aug 2026):** JSONL at a stable `user://` path. Entries:
  timestamp, severity, message, script/line; run start/end markers; crashes recorded
  with exact output and exit status (a crash must never look like a quiet run). Kept
  separate from the raw stdout log. Run-tool results carry a digest (unique error
  count, warning count, crashed?, log path); the agent pulls the file when the digest
  is not enough. Duplicate-spam collapsing (per-frame errors): back-burner — the
  structured file is greppable/filterable by the agent as ordinary log work.
  *Built:* `user://ai_journal/runtime_errors.jsonl` via the shared journal writer;
  per-error entries from the debugger's existing capture (new
  `runtime_error_reported` signal); run markers from `EditorRunBar` signals; crash
  detection by sampling the child's exit code in `stop_playing()` before the kill
  erases it; `run_digest` in `run_and_screenshot`/`stop_game` results in both loops;
  pull route is `run_editor_script` + `FileAccess`. See
  [runtime_error_log](../architecture/runtime_error_log.md).

Silent wrongness (script succeeds, result is wrong) is not catchable by error
plumbing. It is covered by the read API (verify in-script or with a follow-up
read-only script) and by visual capture. The [SCENE UPDATE] diff machinery stays on
the shelf — re-enabling it is a switch-flip if this shows up as a real failure
pattern in testing.

## 6. What happens to the ~60 tools

- **Retired into the cookbook:** all pure editor-API tools (nodes, properties,
  resources, scripts, signals, scenes, settings, files). Their behaviors become
  reference scripts; their tests become the regression suite for the script engine.
- **Kept as real tools:** harness infrastructure that is not an engine operation —
  `run_and_screenshot` / viewport capture (image return path), `preview_asset`,
  `run_project` / `stop_game`, `write_dev_note`, `update_todos`, VCS tools.
- Transition: old tools stay functional on the branch while the prompt steers
  script-first; retire write-tools once the benchmark passes.

## 7. Rollout

1. Dedicated branch.
2. Build `run_editor_script` + Phase A result contract.
3. Phase B static-error check (mostly exists) + the swallowed-errors fix.
4. Phase C runtime error log. **Done** ([runtime_error_log](../architecture/runtime_error_log.md)).
5. **Acceptance benchmark:** the paladin level task — imported tileset → TileSet
   built → background painted → verified visually, well under 10 minutes. We hold a
   recorded failure of exactly this task to compare against.

## 8. Backlog (explicitly not now)

- Screenshots-on-error during runs (with per-frame trap guarded).
- Monitored play / runtime input injection (the weak feedback channel today).
- Duplicate-error collapsing in the runtime log.
- Script-side undo helper API (undo/redo interplay overall is deliberately
  deferred — checkpoint is the rollback story for now).
- Script execution on a separate monitored thread — enables real timeouts and
  stop-the-script; v1 runs on the main thread and accepts the hang risk.
- Remove duplicate transcript persistence — rely on codex rollouts
  ([thoughts.md](../backlog/thoughts.md)).
- Re-enabled scene diffs, if silent wrongness demands it.
