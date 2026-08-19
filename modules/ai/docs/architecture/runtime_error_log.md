# Runtime Error Log

*August 2026. Phase C of [script_execution_plan](../design/script_execution_plan.md) §5.
Organizing rule: push a digest, pull the detail.*

## What it is

Every game run streams its errors into a structured JSONL log at a stable path:

```
user://ai_journal/runtime_errors.jsonl
```

The log lives in the same `ai_journal` directory as `runs.jsonl` and reuses the same
append-only background writer (`ai_journal_writer.cpp`). It is separate from the raw
stdout/Output-panel log. The run tools push a small **digest** of the run into their
results; the agent pulls the file itself when the digest is not enough.

## Entry schema

One JSON object per line. `type` discriminates:

| type | fields |
|---|---|
| `run_start` | `run_id`, `ts` (UTC datetime), `scene`, `mode`, `initiator` (`agent`/`user`), `pid` |
| `error` / `warning` | `run_id`, `ts`, `t_ms` (ms since run start), `message`, `script`, `line`, `function` (source fields only when the error points into project code) |
| `run_end` | `run_id`, `ts`, `duration_ms`, `exit_status`, `exit_code`, `error_count`, `unique_error_count`, `warning_count`, and on crash `output_tail` (the recent Output-panel text — the crash message usually exists only there, because the debugger connection died with the process) |

There is **no duplicate collapsing** (deliberate): an error raised every frame produces
a line every frame. The file is structured, so the agent filters it like any log.
Growth is bounded by simple rotation: when the log exceeds ~4 MB at run start, it is
renamed to `runtime_errors.prev.jsonl` (replacing the previous one) and a fresh file
starts.

## Where the entries come from

- **Errors/warnings**: the game process reports errors to the editor over the debugger
  protocol. `ScriptEditorDebugger` already captures them (`_error_records`, the same
  capture behind `get_structured_errors()`); it now also emits a
  `runtime_error_reported` signal per error, which `AIRuntimeErrorLog`
  (`modules/ai/runtime_error_log.cpp`) turns into a log line as it happens.
- **Run markers**: `AIRuntimeErrorLog` listens to `EditorRunBar`'s `play_pressed` /
  `stop_pressed` signals, so user-initiated runs are bracketed too. The run tools call
  `note_run_requested()` before launching, which marks the run's `initiator` as
  `agent`.

## Crash detection

A crash must never look like a quiet run. The mechanism: every run end funnels through
`EditorRunBar::stop_playing()` (the editor Stop button, `stop_game`, AND the
self-exit path — when the game process dies, the debugger disconnect calls it). At the
top of `stop_playing()`, **before** `EditorRun::stop()` kills the child (`OS::kill()`
erases the OS-level exit bookkeeping), the run bar samples whether the child had
already exited on its own and with what exit code. That yields three exit states:

- `stopped` — the editor killed a live process (user/agent stop). No exit code.
- `exited` — the process quit on its own with exit code 0 (e.g. window closed).
- `crashed` — the process died on its own with a nonzero exit code. The `run_end`
  entry records the exact exit code and the output tail.

Limitation: runs without a local child pid (remote/native deploys) report `stopped`
with no exit code.

## The digest (push)

`run_and_screenshot` and `stop_game` results carry `run_digest`:
`run_id`, `running`, `exit_status` (+ `exit_code`), `crashed`, `error_count`,
`unique_error_count`, `warning_count`, `first_errors` (up to 5 unique errors
verbatim), `duration_ms`, `log_path`. Unique = same message at the same script:line,
matching `get_structured_errors()`. `run_project` results carry `run_id` and the
`runtime_log` path (its digest arrives when the run ends — normally via `stop_game`,
which reports the last run's digest even if the game already died on its own).

## The pull route

The editor and the game resolve `user://` to the same project userdata directory, so
the agent reads the log in-process with `run_editor_script`:

```gdscript
@tool
func run() -> Variant:
    return FileAccess.get_file_as_string("user://ai_journal/runtime_errors.jsonl")
```

(Filter/tail in-script for big logs.) The pull route is stated in the run tools'
descriptions and the system prompt's diagnostics reference.
