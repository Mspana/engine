# Phase 0 Plan — Session Seam (`AIChatSession`)

## Goal

Give the remote layer a stable, UI-independent API for observing and driving AI chats,
without rewriting the 5,700-line transcript/rendering pipeline in `AIStatusPanel`.

## Key realization that changes the design

The original idea doc called for "extract a headless session object; the panel becomes
client #1." A literal reading means moving all persistence + orchestration out of the
panel — a very large, very risky refactor of code that is deeply interleaved with
rendering (e.g. `_on_orchestrator_tool_result` persists *and* updates the tool card in one
pass).

But **the editor process always has the panel** (it is an `EditorPlugin` created at
startup). Nothing in Phases 1–2 requires running with no panel. What the remote layer
actually needs is:

1. read history,
2. observe live events,
3. inject a user message,
4. answer approvals,
5. switch/create chats.

All five can be satisfied by a **seam** rather than a relocation. Full headlessness (agent
running with no editor UI at all) is deferred until something actually needs it.

**Decision:** build `AIChatSession` as a session hub that owns nothing UI-shaped and
exposes exactly the API a remote client needs. The panel registers itself as the executor.
When a future full extraction happens, this API does not change — only the handlers move.
Logged in QUESTIONS.md as Q1.

## Design

### 1. `AIChatStore` emits signals (the observation seam)

The store is already UI-free and every persistence path in the panel already funnels
through it. Adding signals there means **zero changes to the ~20 `append_item` call
sites**:

- `item_appended(int64 ts, Dictionary data)` — emitted by `append_item`
- `items_rewritten()` — emitted by `rewrite_items` / `truncate_*` / `clear_items`
- `chat_changed(String chat_id)` — emitted by `set_chat_id` / `set_file_path`

### 2. `AIChatSession` (new, `modules/ai/session/`) — the command + broadcast seam

Object singleton, editor-only, created in `register_types`. Holds no UI types.

Observation (re-broadcast, so consumers connect to one object):
- `history_changed(String reason)` — appended / rewritten / switched
- `item_appended(int64 ts, Dictionary data)`
- `delta(String kind, String text)` — `kind` is `assistant` or `thinking`
- `run_state_changed(bool running)`
- `status_changed(String text)`
- `approval_changed(Dictionary info)` — empty dict = cleared
- `chat_switched(String chat_id)`

Commands (callable from any consumer; forwarded to the registered executor):
- `submit_user_message(String)`, `cancel_run()`, `respond_approval(String)`
- `switch_chat(String)`, `new_chat()`, `list_chats()`, `get_history()`

State accessors: `get_chat_id()`, `is_running()`, `get_pending_approval()`,
`get_store()`.

### 3. Panel changes (small and mechanical)

- Store is created by the session; panel takes `chat_store = session->get_store()` so all
  existing `chat_store->` code is untouched.
- Panel calls `session->set_executor(...)` with `Callable`s bound to existing methods.
- Panel adds `submit_external_message(String)` (public) = the send-button path, honouring
  the existing queue-while-running behaviour.
- Panel pushes broadcast calls from four existing handlers: assistant delta, thinking
  delta, run-state change, approval show/clear.

Estimated panel diff: ~60 lines. No handler rewrites, no signal reconnection churn.

### 4. Non-goals for Phase 0

- No headless (panel-less) operation.
- No multi-concurrent sessions (remote drives the *active* chat; switching remotely
  switches the panel too — the dual-surface mirroring model used by Claude Code and Happy).
- Legacy `AgenticOrchestrator` path untouched; it persists through the same store, so
  remote observation works there too, but remote *driving* targets harness mode.

## Test plan

- Unit tests (doctest, `extra_suffix=tests`): store signal emission on append / rewrite /
  chat change; session re-broadcast; command forwarding to a stub executor; command
  rejection when no executor is registered.
- Compile the editor build to confirm the panel wiring is intact.
