# Conversation Protocol v2

Specification for message structure, persistence, and API serialization in the Aristotle AI system.

Supersedes the original conversation_protocol.md.

---

## Design Principles

1. **Role says who is talking, type says what kind of content it is.** Messages carry a role (`user`, `assistant`, `tool`). Injected context is a separate category of item types, not a role.
2. **The canonical history is provider-neutral.** Provider adapters translate to wire format. The internal representation never contains OpenAI-specific or Claude-specific shapes.
3. **Everything the model saw is persisted.** If it was in the messages array when the API was called, it's in the history file. Ephemeral elements are UI-only and never enter the item list.
4. **The agentic loop is uniform.** Every iteration of the loop sees the same shape of history — it does not know or care whether this is the first turn, a mid-run follow-up, or a post-cancellation restart.

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

A single assistant message may contain interleaved `text` and `tool_call` blocks. For example, an assistant message with narration followed by a tool call:

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

For cancelled tool calls (tools that were issued but the run was cancelled before execution completed — see §6):

```json
{
  "role": "tool",
  "tool_call_id": "call_def456",
  "content": {
    "status": "cancelled",
    "tool_name": "write_script",
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

### user_injection

```json
{
  "type": "user_injection",
  "content": "Actually, also make the player double-jump",
  "images": [],
  "created_at": 1743751240000,
  "delivered": false
}
```

`delivered` is flipped to `true` when the injection is consumed by the agentic loop. On persistence, both delivered and undelivered injections are stored. On reload, undelivered injections are re-queued.

---

## 4. Placement Rules

Context injections have deterministic placement in the messages array, enforced by the provider adapter at serialization time — not by the orchestrator doing string surgery.

| Type | Placement | Always included? |
|------|-----------|-----------------|
| `engine_state` | Before the last user message | Yes — most recent one only |
| `todo_state` | After all messages | Yes, if tasks exist |
| `scene_snapshot` | With or after `engine_state` | Only if fresh (within `stale_after_ms` of current time) |
| `user_injection` | As a `user` message at the point it was received in history | Yes, once delivered |

The provider adapter walks the item list, filters and places injections according to these rules, and serializes each to the target API's format.

---

## 5. Provider Adapter (Wire Format Translation)

The provider adapter is the only layer that knows about API-specific message shapes. It translates the canonical item list to the target provider's format.

### OpenAI-compatible (current default)

| Canonical Item | Wire Format |
|---------------|-------------|
| `user` message | `{ role: "user", content: "..." }` wrapped in `<user_message>` tags with timestamp and injection guard |
| `assistant` message | `{ role: "assistant", content: "...", tool_calls: [...] }` — text and tool_call blocks split into OpenAI's `content` string + `tool_calls` array |
| `tool` result | `{ role: "tool", tool_call_id: "...", content: "..." }` |
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
| `tool` result | `{ role: "user", content: [{ type: "tool_result", tool_use_id: "...", content: "..." }] }` — note: wrapped in a user turn |
| Context injections | `{ role: "user", content: [{ type: "text", text: "..." }] }` |
| System prompt | Top-level `system` parameter |

### Codex / OpenAI Responses API (future)

| Canonical Item | Wire Format |
|---------------|-------------|
| `user` message | `ResponseItem::Message { role: "user", ... }` |
| `assistant` message | `ResponseItem::Message { role: "assistant", ... }` |
| `tool` result | `ResponseItem::FunctionCallOutput { call_id, output }` — note: dedicated item type, not a role |
| Context injections | `ResponseItem::Message { role: "developer", ... }` or `{ role: "user", ... }` depending on type |
| System prompt | Top-level `instructions` field |

---

## 6. Cancellation

When a run is cancelled:

1. **Let in-flight tool calls finish.** If a tool was dispatched to the engine and is executing, wait for it to return. Record its result as a normal `tool` item.
2. **Record synthetic results for unexecuted tool calls.** If the assistant requested tool calls that were never dispatched (e.g., the second call in a batch when cancel hit during the first), record a `tool` item with `status: "cancelled"`.
3. **No orphan tool calls.** Every `tool_call` block in an assistant message must have a corresponding `tool` result item in history. This is a hard invariant — without it, the conversation history is malformed for the next API call.
4. **No cancellation message in history.** The cancellation itself is not an item. The model infers interruption from the pattern: an assistant message with tool calls, followed by tool results (some possibly cancelled), followed by a new user message — with no final assistant text in between.
5. **UI may show an ephemeral cancellation indicator** ("Cancelled. Tell me what to do next.") that is not persisted.

### Cancellation with in-flight user message

When a user sends a message while the AI is mid-run:

1. Set the run as cancelled.
2. Let any in-flight tool calls finish and record their results.
3. Record synthetic cancelled results for unexecuted tool calls.
4. Insert the user's message as a normal `user` item (or `user_injection` if received mid-turn) at its chronological position.
5. Start a new run. The model sees: `... → assistant (with tool_calls) → tool (results, some cancelled) → user (new message)` and continues naturally.

The agentic loop does not distinguish this from any other turn — it sees history ending with a user message and responds.

---

## 7. Agentic Loop (Uniform Shape)

Every iteration of the agentic loop follows the same contract:

1. Build the messages array from the canonical item list (via provider adapter).
2. Send to the model.
3. Receive response: assistant message with text, tool calls, or both.
4. Record the assistant message to history.
5. If tool calls: execute each, record `tool` results to history, loop back to step 1.
6. If no tool calls (finish_reason == "stop"): run is complete.

At step 1, pending `user_injection` items are marked as `delivered` and included in their chronological position. The loop does not need special handling for injections — they are already in the item list.

At each checkpoint (before step 2), the loop checks for cancellation. If cancelled, it follows the cancellation protocol in §6.

---

## 8. Persistence

### Format

Each chat is a **JSONL file**: one JSON object per line, append-only.

**Location:** `user://ai_chat/<chat_id>.jsonl`

Each line is a timestamped item:

```jsonl
{"ts":1743751234567,"item":{"role":"user","content":"Make the player jump higher","images":[]}}
{"ts":1743751235000,"item":{"type":"engine_state","game_running":false,"error_count":0,"errors":[],"screenshot_path":null,"timestamp":1743751235000}}
{"ts":1743751236000,"item":{"role":"assistant","content":[{"type":"text","text":"I'll check your player script."},{"type":"tool_call","id":"call_abc","name":"read_script","args":{"file_path":"res://Player.gd"}}]}}
{"ts":1743751237000,"item":{"role":"tool","tool_call_id":"call_abc","content":{"status":"success","tool_name":"read_script","args":{"file_path":"res://Player.gd"},"result":{"content":"extends CharacterBody2D..."}}}}
{"ts":1743751238000,"item":{"role":"assistant","content":[{"type":"text","text":"Done! I updated JUMP_VELOCITY to -500."}]}}
```

Items are appended as they occur. The file is never rewritten during normal operation.

### What is NOT persisted

- Cancellation labels
- Separators between API rounds
- Token count labels
- "Thinking..." pending animation
- "Context is full" notices
- UI display metadata (`_tool_result_data`, screenshot thumbnails, collapse state)

### Reconstruction on reload

Walk the JSONL lines in order, rebuilding the item list. The UI renderer maps each item to its display widget:

- `user` → right-aligned bubble
- `assistant` → left-aligned bubble (render content blocks: text as markdown, tool_call blocks as collapsible entries)
- `tool` → `ToolCollapsibleEntry`
- `engine_state`, `todo_state`, `scene_snapshot` → not displayed (or displayed as a subtle context indicator, design TBD)
- `user_injection` with `delivered: false` → re-queue for delivery

---

## 9. Context Management (Sliding Window)

`_build_model_messages()` applies a character budget before serialization:

**Budget:** `(provider_context_window_tokens - 10000) * 4` characters. Fallback: 120,000 characters.

**Algorithm:**
1. Walk the item list backwards, accumulating content size.
2. When budget is exceeded, set `start_index` and drop everything before it.
3. **Always retain** the most recent `engine_state` and `todo_state`, even if they fall outside the window.
4. Ensure no orphan tool calls or results at the truncation boundary (if a `tool` result is included, its corresponding assistant `tool_call` must also be included, and vice versa).
5. Pass the windowed list to the provider adapter for serialization.

**Context exhaustion:**
- `context_was_truncated = true` — informational flag, checked after run completion.
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

---

## 12. Multi-Chat Support

Each conversation is an independent JSONL file.

**Chat ID format:** `chat_<unix_timestamp_ms>`

**Operations:**
- `_new_chat()` — generates new ID, creates empty file, clears UI
- `_switch_to_chat(id)` — loads that chat's JSONL, rebuilds item list and UI
- `_delete_chat(id)` — confirmation dialog → deletes file, switches to newest remaining chat

---

## 13. Example: Full Multi-Turn Conversation

A complete conversation showing a user request, two sequential tool calls, a cancellation with in-flight user message, and recovery:

```jsonl
{"ts":1743751234567,"item":{"role":"user","content":"Make the player jump higher","images":[]}}
{"ts":1743751235000,"item":{"type":"engine_state","game_running":false,"error_count":0,"errors":[],"screenshot_path":null,"timestamp":1743751235000}}
{"ts":1743751236000,"item":{"role":"assistant","content":[{"type":"text","text":"I'll read the player script first to find the current jump velocity."},{"type":"tool_call","id":"call_001","name":"read_script","args":{"file_path":"res://Player.gd"}}]}}
{"ts":1743751237000,"item":{"role":"tool","tool_call_id":"call_001","content":{"status":"success","tool_name":"read_script","args":{"file_path":"res://Player.gd"},"result":{"content":"extends CharacterBody2D\nconst JUMP_VELOCITY = -300.0"}}}}
{"ts":1743751238000,"item":{"role":"assistant","content":[{"type":"text","text":"Found it — JUMP_VELOCITY is -300. I'll increase it to -500."},{"type":"tool_call","id":"call_002","name":"write_script","args":{"file_path":"res://Player.gd","content":"extends CharacterBody2D\nconst JUMP_VELOCITY = -500.0"}}]}}
{"ts":1743751239000,"item":{"role":"tool","tool_call_id":"call_002","content":{"status":"success","tool_name":"write_script","args":{"file_path":"res://Player.gd","content":"..."},"result":{"bytes_written":45}}}}
{"ts":1743751240000,"item":{"role":"assistant","content":[{"type":"text","text":"Done! I updated JUMP_VELOCITY from -300 to -500 in Player.gd. The player will now jump significantly higher. Run the game to test it out."}]}}
{"ts":1743751250000,"item":{"role":"user","content":"Now add a double jump","images":[]}}
{"ts":1743751251000,"item":{"type":"engine_state","game_running":true,"error_count":0,"errors":[],"screenshot_path":"user://screenshots/frame_001.png","timestamp":1743751251000}}
{"ts":1743751252000,"item":{"role":"assistant","content":[{"type":"text","text":"I'll read the current player script to add double jump logic."},{"type":"tool_call","id":"call_003","name":"read_script","args":{"file_path":"res://Player.gd"}},{"type":"tool_call","id":"call_004","name":"read_script","args":{"file_path":"res://Main3D.tscn"}}]}}
{"ts":1743751253000,"item":{"role":"tool","tool_call_id":"call_003","content":{"status":"success","tool_name":"read_script","args":{"file_path":"res://Player.gd"},"result":{"content":"extends CharacterBody2D\nconst JUMP_VELOCITY = -500.0"}}}}
{"ts":1743751253500,"item":{"role":"tool","tool_call_id":"call_004","content":{"status":"cancelled","tool_name":"read_script","args":{"file_path":"res://Main3D.tscn"},"reason":"user_cancelled_run"}}}
{"ts":1743751254000,"item":{"role":"user","content":"Actually, make it a triple jump instead","images":[]}}
{"ts":1743751255000,"item":{"role":"assistant","content":[{"type":"text","text":"Got it — triple jump instead. I already have the player script from the last read. I'll add a jump counter that allows up to 3 jumps before requiring a landing."},{"type":"tool_call","id":"call_005","name":"write_script","args":{"file_path":"res://Player.gd","content":"extends CharacterBody2D\nconst JUMP_VELOCITY = -500.0\nvar jump_count := 0\nconst MAX_JUMPS := 3\n..."}}]}}
```

---

## Migration from v1

| v1 Concept | v2 Equivalent |
|-----------|---------------|
| `role: "thinking"` | Removed. If thinking/reasoning is reintroduced, it becomes a content block type inside an `assistant` message. |
| `role: "narration"` | `assistant` message with `text` content block. No separate role — narration is just assistant text emitted mid-turn. |
| `[GAME SESSION]` string injection | `engine_state` context injection item |
| `[CURRENT_TODOS]` string injection | `todo_state` context injection item |
| `_pending_user_injection` | `user_injection` context injection item |
| Single JSON file per chat | JSONL file per chat (append-only) |
| Role remapping in `_build_model_messages` | Provider adapter handles all wire format translation |
| `_tool_result_data` UI metadata | UI-only, reconstructed on render from the persisted `tool` item's content |
