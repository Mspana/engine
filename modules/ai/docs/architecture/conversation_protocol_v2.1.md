# Conversation Protocol v2.1

Specification for message structure, persistence, and API serialization in the Aristotle AI system.

Supersedes conversation_protocol_v2.md.

**Changes from v2:**
- §6 Cancellation: enforcement via execution logic (not truncation walkback); synthetic cancelled results; system prompt guidance added
- §8 Persistence: JSONL rewrite rule on rewind; `delivered` inference rule for `user_injection`; backward compat not required (MVP)
- §9 Context management: simplified truncation — no orphan-safe walkback needed
- §13 Migration: v1 cross-session history bug documented explicitly

---

## Design Principles

1. **Role says who is talking, type says what kind of content it is.** Messages carry a role (`user`, `assistant`, `tool`). Injected context is a separate category of item types, not a role.
2. **The canonical history is provider-neutral.** Provider adapters translate to wire format. The internal representation never contains OpenAI-specific or Claude-specific shapes.
3. **Everything the model saw is persisted.** If it was in the messages array when the API was called, it's in the history file. Ephemeral elements are UI-only and never enter the item list.
4. **The agentic loop is uniform.** Every iteration of the loop sees the same shape of history — it does not know or care whether this is the first turn, a mid-run follow-up, or a post-cancellation restart.
5. **The no-orphan invariant is enforced at write time, not read time.** The execution layer guarantees every `assistant` message with tool calls has a corresponding `tool` result for every call before the next item is written. Truncation can cut at any item boundary safely.

---

## 1. Item Types

The conversation history is an ordered list of **items**. Each item is one of two categories: **messages** (role-bearing turns in the conversation) or **context injections** (structured engine/project state).

### Messages

| Role | Description |
|------|-------------|
| `user` | Human-typed message. May include images (base64 PNG references). Also used for in-flight user messages injected mid-run. |
| `assistant` | Model output. Content is an ordered array of content blocks (see §2). A single assistant message may contain text, tool calls, or both. |
| `tool` | Tool execution result. Paired to an assistant's tool call by `tool_call_id`. Content is the JSON-stringified execution result. |

There are no other message roles. `thinking` and `narration` from protocol v1 are removed — see §2 for how that content is represented.

### Context Injections

These are first-class items in the history list, distinct from messages. Each has a `type` field and a structured payload. They are persisted and sent to the model (via the provider adapter), but they are not role-bearing conversation turns.

| Type | Description | Replaces (v1) |
|------|-------------|---------------|
| `engine_state` | Game session status: running/stopped, error count, error details, optional screenshot path. | `[GAME SESSION]` string block |
| `todo_state` | Current task list as structured data: array of `{ id, text, status }`. | `[CURRENT_TODOS]` string block |
| `scene_snapshot` | Scene tree state, node properties, project settings. Carries a freshness timestamp; stale snapshots may be excluded from model input by the provider adapter. | (new — previously folded into game session or ad-hoc user messages) |
| `user_injection` | A user message sent while the AI was mid-run. Stored at the point it was received, delivered to the model at the next natural checkpoint in the agentic loop. | `_pending_user_injection` side channel |

---

## 2. Content Blocks

Assistant messages use an ordered `content` array of typed blocks rather than a flat string. This replaces the v1 pattern of separate `thinking` and `narration` roles.

### Assistant content block types

| Block Type | Description |
|------------|-------------|
| `text` | Model-generated text. Final responses, mid-turn narration, and planning text are all `text` blocks. |
| `tool_call` | A request to execute a tool. Contains `id`, `name`, `args`. Multiple `tool_call` blocks may appear in a single assistant message (parallel tool use). |

A single assistant message may contain interleaved `text` and `tool_call` blocks. For example:

```json
{
  "role": "assistant",
  "content": [
    { "type": "text", "text": "I'll read the player script to check the jump velocity." },
    { "type": "tool_call", "id": "call_abc123", "name": "read_script", "args": { "file_path": "res://Player.gd" } }
  ]
}
```

### Tool result content

Tool messages carry JSON-stringified execution results. The `status` field indicates outcome:

```json
{
  "role": "tool",
  "tool_call_id": "call_abc123",
  "content": {
    "status": "success",
    "tool_name": "read_script",
    "args": { "file_path": "res://Player.gd" },
    "result": { "content": "extends CharacterBody2D\n..." }
  }
}
```

For tool calls that were dispatched to the assistant message but not executed before cancellation:

```json
{
  "role": "tool",
  "tool_call_id": "call_def456",
  "content": {
    "status": "cancelled",
    "tool_name": "update_script",
    "args": { "file_path": "res://Player.gd", "content": "..." },
    "reason": "user_cancelled_run"
  }
}
```

### User message content

User messages may be a plain string or an array of content blocks when images are attached:

```json
{
  "role": "user",
  "content": [
    { "type": "text", "text": "Why is my character falling through the floor?" },
    { "type": "image", "source": "base64", "data": "iVBOR..." }
  ]
}
```

---

## 3. Context Injection Schemas

### engine_state

```json
{
  "type": "engine_state",
  "game_running": false,
  "error_count": 0,
  "errors": [],
  "screenshot_path": null,
  "timestamp": 1743751234567
}
```

### todo_state

```json
{
  "type": "todo_state",
  "tasks": [
    { "id": "task-1", "text": "Read player script", "status": "done" },
    { "id": "task-2", "text": "Update jump velocity", "status": "in_progress" },
    { "id": "task-3", "text": "Test changes", "status": "pending" }
  ],
  "timestamp": 1743751234567
}
```

### scene_snapshot

```json
{
  "type": "scene_snapshot",
  "scene_path": "res://Main3D.tscn",
  "node_tree": { "...": "structured scene tree data" },
  "project_settings": { "application/run/main_scene": "res://Main3D.tscn" },
  "timestamp": 1743751234567,
  "stale_after_ms": 30000
}
```

A `scene_snapshot` is considered stale if `current_time_ms - timestamp > stale_after_ms`. Stale snapshots are excluded by the provider adapter when building the API request. This means snapshots from a previous session are always excluded on the next session start — which is the intended behavior (the engine will inject a fresh `engine_state` at the start of each new run).

### user_injection

```json
{
  "type": "user_injection",
  "content": "Actually, also make the player double-jump",
  "images": [],
  "created_at": 1743751240000
}
```

`delivered` state is inferred at load time, not stored. See §8 for the inference rule.

---

## 4. Placement Rules

Context injections have deterministic placement in the messages array, enforced by the provider adapter at serialization time — not by the orchestrator doing string surgery.

| Type | Placement | Always included? |
|------|-----------|-----------------|
| `engine_state` | Before the last user message | Yes — most recent one only |
| `todo_state` | After all messages | Yes, if tasks exist |
| `scene_snapshot` | With or after `engine_state` | Only if fresh (within `stale_after_ms` of current time) |
| `user_injection` | As a `user` message at its chronological position in the item list | Yes, once delivered |

The provider adapter walks the item list, filters and places injections according to these rules, and serializes each to the target API's format.

---

## 5. Provider Adapter (Wire Format Translation)

The provider adapter is the only layer that knows about API-specific message shapes. It translates the canonical item list to the target provider's format.

### OpenAI-compatible (current default)

| Canonical Item | Wire Format |
|---------------|-------------|
| `user` message | `{ role: "user", content: "..." }` wrapped in `<user_message>` tags with timestamp and injection guard |
| `assistant` message | `{ role: "assistant", content: "...", tool_calls: [...] }` — `text` blocks joined into OpenAI's `content` string; `tool_call` blocks serialized into the `tool_calls` array |
| `tool` result | `{ role: "tool", tool_call_id: "...", content: "{...}" }` |
| `engine_state` | `{ role: "user", content: "[GAME SESSION]\nStatus: ...\nErrors: ..." }` |
| `todo_state` | `{ role: "user", content: "[CURRENT_TODOS]\n..." }` |
| `scene_snapshot` | `{ role: "user", content: "[SCENE STATE]\n..." }` or omitted if stale |
| `user_injection` | `{ role: "user", content: "<user_message>...</user_message>" }` |
| System prompt | Top-level `system` field (not in messages array) |

### Claude Messages API (future)

| Canonical Item | Wire Format |
|---------------|-------------|
| `user` message | `{ role: "user", content: [{ type: "text", text: "..." }] }` |
| `assistant` message | `{ role: "assistant", content: [{ type: "text", ... }, { type: "tool_use", ... }] }` |
| `tool` result | `{ role: "user", content: [{ type: "tool_result", tool_use_id: "...", content: "..." }] }` — wrapped in a user turn |
| Context injections | `{ role: "user", content: [{ type: "text", text: "..." }] }` |
| System prompt | Top-level `system` parameter |

### Codex / OpenAI Responses API (future)

| Canonical Item | Wire Format |
|---------------|-------------|
| `user` message | `ResponseItem::Message { role: "user", ... }` |
| `assistant` message | `ResponseItem::Message { role: "assistant", ... }` |
| `tool` result | `ResponseItem::FunctionCallOutput { call_id, output }` — dedicated item type, not a role |
| Context injections | `ResponseItem::Message { role: "developer", ... }` or `{ role: "user", ... }` depending on type |
| System prompt | Top-level `instructions` field |

---

## 6. Cancellation

### The no-orphan invariant

**Every `tool_call` block in an assistant message must have a corresponding `tool` result item before any subsequent item is written.** This is enforced by execution logic, not by the truncation algorithm.

Practically: the orchestrator does not write the next item (another assistant message, a new user message, or a context injection) until all tool results for the current assistant message are present in the item list — either as real results or as synthetic cancelled results.

### Cancellation procedure

When a run is cancelled:

1. **Complete any tool that has already started executing.** If the engine dispatched a tool call and is waiting for the result, wait for it and record the real result.
2. **Write synthetic `cancelled` results for any tool calls that were not dispatched.** If the assistant message contained 3 tool calls and only 1 was dispatched before cancellation, write synthetic cancelled results for calls 2 and 3 before stopping.
3. **Do not write a cancellation message to the item list.** The cancellation itself is not an item. The model infers interruption from the pattern: `assistant (with tool_calls) → tool results (some cancelled) → user (next message)`.
4. **Show an ephemeral cancellation indicator in the UI** ("Cancelled. Tell me what to do next.") that is not persisted to the item list.

### Async run_and_screenshot during cancellation

`run_and_screenshot` spans multiple engine frames via a timer state machine. If cancellation is requested during this tool:

- If in `POLL_START` or `WAIT_VISUAL` phase: cancel the timer, write a synthetic cancelled result for the `run_and_screenshot` tool call, then stop.
- If in `AWAIT_CAPTURE` phase: the screenshot callback may still arrive. Let it arrive (max 10s timeout already applies), record the real result, then stop. If the timeout fires first, write a synthetic cancelled result.

The `_async_rns_tool_call_id` field already tracks which call ID to use for the synthetic result.

> **Test case:** cancel during `AWAIT_CAPTURE` (after screenshot trigger, before callback). Verify: (a) a `tool` item appears in the history with either the real screenshot result or `status: "cancelled"`, (b) no orphan tool call exists in the file, (c) the next run starts cleanly.

### System prompt guidance for cancelled runs

The system prompt must include guidance for how the model should behave when it encounters `status: "cancelled"` tool results at the start of a turn. The intended behavior is:

> When you see `status: "cancelled"` tool results in the conversation history, acknowledge to the user that some work was interrupted and ask them how to proceed. Do not silently re-attempt the cancelled work — the user may have changed their mind or want to take a different approach. Example: "It looks like we were mid-way through [task] when you stopped the run. Would you like me to continue, or take a different approach?"

This applies when cancellation left unexecuted work. If all tools completed normally and the user simply stopped the run (no cancelled results), no special acknowledgment is needed.

---

## 7. Agentic Loop (Uniform Shape)

Every iteration of the agentic loop follows the same contract:

1. Build the messages array from the canonical item list (via provider adapter).
2. Send to the model.
3. Receive response: assistant message with text, tool calls, or both.
4. **Immediately write the assistant message to the item list.**
5. If tool calls: execute each, write each `tool` result to the item list as it completes, loop back to step 1.
6. If no tool calls (finish_reason == "stop"): run is complete.

Step 4 and 5 together maintain the no-orphan invariant: the assistant message is written first, then each tool result is written as it finishes. If cancellation interrupts step 5, synthetic cancelled results fill the gap before the loop exits.

At each checkpoint (before step 2), the loop checks for cancellation. If cancelled, it follows the cancellation protocol in §6.

Pending `user_injection` items are marked as delivered (by inference — see §8) once a new assistant message is written that follows them in the item list.

---

## 8. Persistence

### Format

Each chat is a **JSONL file**: one JSON object per line, append-only during normal operation.

**Location:** `user://ai_chat/<chat_id>.jsonl`

Each line is a timestamped item:

```jsonl
{"ts":1743751234567,"item":{"role":"user","content":"Make the player jump higher","images":[]}}
{"ts":1743751235000,"item":{"type":"engine_state","game_running":false,"error_count":0,"errors":[],"screenshot_path":null,"timestamp":1743751235000}}
{"ts":1743751236000,"item":{"role":"assistant","content":[{"type":"text","text":"I'll check your player script."},{"type":"tool_call","id":"call_abc","name":"read_script","args":{"file_path":"res://Player.gd"}}]}}
{"ts":1743751237000,"item":{"role":"tool","tool_call_id":"call_abc","content":{"status":"success","tool_name":"read_script","result":{"content":"extends CharacterBody2D..."}}}}
{"ts":1743751238000,"item":{"role":"assistant","content":[{"type":"text","text":"Done! I updated JUMP_VELOCITY to -500."}]}}
```

### Append vs. rewrite rule

- **During a run:** append-only. Each new item is appended as it is produced. The file is never read back during a run.
- **On explicit rewind (checkpoint restore or edit truncation):** the file is rewritten in full, keeping only items up to the truncation point. This is a rare, explicit user action — the performance cost is acceptable.

Checkpoints are stored in a parallel `user://ai_chat/<chat_id>.meta.json` file (not inline in the JSONL) to avoid complicating the append stream. The meta file contains checkpoint records and is rewritten on each checkpoint creation or deletion.

### `user_injection` delivery state

`user_injection` items do not carry a `delivered` flag. Delivery is inferred at load time:

> A `user_injection` item is considered **delivered** if an `assistant` message with a `ts` greater than the injection's `ts` appears anywhere later in the file.

On reload, undelivered injections (those with no subsequent assistant message) are re-queued for delivery on the next run.

### What is NOT persisted

- Cancellation labels ("Cancelled. Tell me what to do next.")
- Separators between API rounds
- Token count labels
- "Thinking..." pending animation
- "Context is full" notices
- UI display metadata (collapse state, screenshot thumbnails, token estimates)

### Reconstruction on reload

Walk the JSONL lines in order, rebuilding the item list. The UI renderer maps each item to its display widget:

- `user` → right-aligned bubble
- `assistant` → left-aligned bubble (render content blocks: `text` as markdown, `tool_call` blocks as collapsible `ToolCollapsibleEntry` widgets)
- `tool` → `ToolCollapsibleEntry` (paired with the preceding assistant message's `tool_call` block by `tool_call_id`)
- `engine_state`, `todo_state`, `scene_snapshot` → not displayed (context indicator, design TBD)
- `user_injection` (undelivered) → re-queued; (delivered) → rendered as a user bubble at its position

### v1 compatibility

Not required. Existing `.json` chat files will not be readable by the v2.1 implementation. This is accepted for the MVP. The `list_chat_ids()` method should look for `.jsonl` files only. Old `.json` files can be left in place and ignored.

---

## 9. Context Management (Sliding Window)

`_build_model_messages()` applies a character budget before serialization:

**Budget:** `(provider_context_window_tokens - 10000) * 4` characters. Fallback: 120,000 characters.

**Algorithm:**
1. Walk the item list backwards, accumulating content size.
2. When budget is exceeded, set `start_index` to the current position and stop.
3. Pass items from `start_index` to end to the provider adapter.

**No orphan-safe walkback is required.** Because the execution layer maintains the no-orphan invariant at write time (§6), every position in the JSONL file is a valid cut point — there are never orphaned tool calls or results in the stored file. Truncation at any `start_index` is safe.

**Always retain:** the most recent `engine_state` and `todo_state` items, even if they fall outside the sliding window. These are injected by the provider adapter regardless of the window.

**Context exhaustion:**
- `context_was_truncated = true` — informational, checked after run completion.
- `context_exhausted = true` — blocks further sends, emits an ephemeral "Context is full" notice in the UI.

---

## 10. Guardrails

| Guardrail | Limit | Checked at |
|-----------|-------|-----------|
| Model turns per run | 20 | Before each API call |
| Tool calls per response | 12 | On response processing |
| Tool calls per run | 50 | Before each API call |
| Context window | `(window_tokens - 10k) * 4` chars | During message building |
| Screenshot timeout (launch) | 8 seconds | Tool execution |
| Screenshot timeout (capture) | 10 seconds | Tool execution |

---

## 11. System Prompt

Source: `modules/ai/system_prompt.inc`

Sent as a top-level field on the API request (`system` for OpenAI/Claude, `instructions` for Codex). Never stored in the chat history. Never included in the item list.

The system prompt is a static string. Dynamic context (game state, TODOs, scene data) is delivered via context injection items, not by modifying the system prompt.

**Required guidance sections:**
- Standard behavioral rules (existing)
- Cancelled tool call acknowledgment (see §6 — model must acknowledge interrupted work and ask user how to proceed)

---

## 12. Multi-Chat Support

Each conversation is an independent JSONL file.

**Chat ID format:** `chat_<unix_timestamp_ms>`

**File layout:**
- `user://ai_chat/<chat_id>.jsonl` — item stream (append-only during runs, rewritten on rewind)
- `user://ai_chat/<chat_id>.meta.json` — checkpoints and chat metadata (rewritten on any checkpoint change)

**Operations:**
- `_new_chat()` — generates new ID, creates empty `.jsonl`, clears UI
- `_switch_to_chat(id)` — loads that chat's `.jsonl`, rebuilds item list and UI
- `_delete_chat(id)` — confirmation dialog → deletes both `.jsonl` and `.meta.json`, switches to newest remaining chat

---

## 13. Example: Full Multi-Turn Conversation

A complete conversation showing a user request, two sequential tool calls, a cancellation with in-flight user message, and recovery:

```jsonl
{"ts":1743751234567,"item":{"role":"user","content":"Make the player jump higher","images":[]}}
{"ts":1743751235000,"item":{"type":"engine_state","game_running":false,"error_count":0,"errors":[],"screenshot_path":null,"timestamp":1743751235000}}
{"ts":1743751236000,"item":{"role":"assistant","content":[{"type":"text","text":"I'll read the player script first to find the current jump velocity."},{"type":"tool_call","id":"call_001","name":"read_script","args":{"file_path":"res://Player.gd"}}]}}
{"ts":1743751237000,"item":{"role":"tool","tool_call_id":"call_001","content":{"status":"success","tool_name":"read_script","result":{"content":"extends CharacterBody2D\nconst JUMP_VELOCITY = -300.0"}}}}
{"ts":1743751238000,"item":{"role":"assistant","content":[{"type":"text","text":"Found it — JUMP_VELOCITY is -300. I'll increase it to -500."},{"type":"tool_call","id":"call_002","name":"update_script","args":{"file_path":"res://Player.gd","old_string":"JUMP_VELOCITY = -300.0","new_string":"JUMP_VELOCITY = -500.0"}}]}}
{"ts":1743751239000,"item":{"role":"tool","tool_call_id":"call_002","content":{"status":"success","tool_name":"update_script","result":{"bytes_written":45}}}}
{"ts":1743751240000,"item":{"role":"assistant","content":[{"type":"text","text":"Done! I updated JUMP_VELOCITY from -300 to -500 in Player.gd. Run the game to test it out."}]}}
{"ts":1743751250000,"item":{"role":"user","content":"Now add a double jump","images":[]}}
{"ts":1743751251000,"item":{"type":"engine_state","game_running":true,"error_count":0,"errors":[],"screenshot_path":"user://screenshots/frame_001.png","timestamp":1743751251000}}
{"ts":1743751252000,"item":{"role":"assistant","content":[{"type":"text","text":"I'll read the current player script and check the scene to plan the double jump implementation."},{"type":"tool_call","id":"call_003","name":"read_script","args":{"file_path":"res://Player.gd"}},{"type":"tool_call","id":"call_004","name":"list_nodes","args":{"root_path":"."}}]}}
{"ts":1743751253000,"item":{"role":"tool","tool_call_id":"call_003","content":{"status":"success","tool_name":"read_script","result":{"content":"extends CharacterBody2D\nconst JUMP_VELOCITY = -500.0"}}}}
{"ts":1743751253500,"item":{"role":"tool","tool_call_id":"call_004","content":{"status":"cancelled","tool_name":"list_nodes","args":{"root_path":"."},"reason":"user_cancelled_run"}}}
{"ts":1743751254000,"item":{"role":"user","content":"Actually, make it a triple jump instead","images":[]}}
{"ts":1743751255000,"item":{"role":"assistant","content":[{"type":"text","text":"It looks like we were in the middle of adding a double jump when you stopped the run — I had started reading your player script but the scene inspection was cancelled. You'd like a triple jump instead. I already have the player script content from the read that completed. I'll add a jump counter allowing up to 3 jumps before requiring a landing."},{"type":"tool_call","id":"call_005","name":"update_script","args":{"file_path":"res://Player.gd","old_string":"const JUMP_VELOCITY = -500.0","new_string":"const JUMP_VELOCITY = -500.0\nvar jump_count := 0\nconst MAX_JUMPS := 3"}}]}}
```

Note: `call_004` (`list_nodes`) was in the assistant message's tool_calls list but was not dispatched before cancellation. A synthetic cancelled result is written before the next user message, maintaining the no-orphan invariant.

---

## 14. Migration from v1

**Backward compatibility is not required for the MVP.** Existing `.json` chat files are abandoned. The `list_chat_ids()` implementation reads `.jsonl` files only.

| v1 Concept | v2.1 Equivalent |
|-----------|----------------|
| `role: "thinking"` | Removed. Legacy role from pre-native-tool-calling system; no longer actively written. If reasoning blocks are introduced (e.g., Claude thinking tokens), they become a `text` content block in the assistant message. |
| `role: "narration"` | `assistant` message with one or more `text` content blocks. No separate role. |
| `[GAME SESSION]` string injection | `engine_state` context injection item |
| `[CURRENT_TODOS]` string injection | `todo_state` context injection item |
| `_pending_user_injection` | `user_injection` context injection item |
| Single JSON file per chat | JSONL item stream (`.jsonl`) + metadata file (`.meta.json`) |
| Role remapping in `_build_model_messages` | Provider adapter handles all wire format translation |
| `_tool_result_data` UI metadata | UI-only, reconstructed on render from the `tool` item's content + the paired `tool_call` block from the preceding assistant message |
| Assistant messages with `tool_calls` not persisted (v1 bug) | Persisted as `assistant` items with `tool_call` content blocks |
| Tool results stored without `tool_call_id` (v1 bug) | Stored with `tool_call_id` as a top-level field on the `tool` item |
| No synthetic cancelled results on cancel (v1 bug) | Synthetic cancelled results written before loop exits |
| Rewind rewrites full JSON file | Rewind rewrites JSONL file; normal operation appends |
