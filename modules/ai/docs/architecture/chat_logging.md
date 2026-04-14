# Chat Logging System

How Aristotle persists and reconstructs AI conversations across editor sessions.

---

## 1. Goal

The AI assistant runs multi-turn agentic loops where a single user request can trigger dozens of tool calls across multiple API round-trips. The logging system serves three purposes:

1. **Cross-session continuity.** When the editor restarts, the full conversation — including intermediate tool calls and their results — is available to the model. Without this, the AI loses all context on restart and can't reference work it did minutes ago.

2. **Structural correctness.** The OpenAI tool-calling API requires strict message pairing: every `tool_calls` array in an assistant message must have a matching `tool` result message with the same `tool_call_id`. If any pair is missing, the API rejects the request. The logging system enforces this invariant at write time so that any prefix of the file is a valid conversation history.

3. **Browsability.** JSONL files are human-readable, line-oriented, and work with standard tools (`grep`, `jq`, text editors). A Windows junction from `Documents/Godot AI Chats/<project>/` makes them easy to find without digging through AppData.

---

## 2. How It Works

### Storage layout

```
%APPDATA%/Godot/app_userdata/<project>/ai_chat/
├── chat_1775348423238.jsonl        # conversation stream (append-only)
├── chat_1775348423238.meta.json    # checkpoints for rewind
├── chat_1775418614055.jsonl
├── chat_1775418614055.meta.json
└── ...

~/Documents/Godot AI Chats/
└── <project name>/                 # junction → ai_chat/ above
```

Each chat gets a `.jsonl` file (the item stream) and a `.meta.json` file (checkpoint metadata for the rewind system). The chat ID is `chat_<unix_ms>` based on creation time.

### Item format

Every line in the JSONL file is a single JSON object:

```json
{"ts": <unix_ms>, "item": { ... }}
```

`ts` is the wall-clock timestamp when the item was written. `item` is one of:

| Item type | Identified by | Description |
|-----------|--------------|-------------|
| User message | `role: "user"` | The human's typed text. May include `images` array of base64 PNGs. |
| Assistant message | `role: "assistant"` | Model output. `content` is an array of typed blocks — `text` and `tool_call`. |
| Tool result | `role: "tool"` | Execution result for one tool call. Keyed to its assistant's `tool_call` block by `tool_call_id`. |
| Model info | `type: "model_info"` | Written before each run with `model_id` and `provider` fields. Identifies which model handled the turn. |
| Context injection | `type: "engine_state"` etc. | Structured engine/project state. Written to JSONL for the record but currently injected at runtime by the orchestrator, not read back from the file. |

### Write protocol

Items are written in strict order during an agentic run:

```
user message           ← user hits Send
  assistant message    ← API response received, BEFORE tools execute
    tool result        ← each tool completes (or is synthetically cancelled)
    tool result
  assistant message    ← next API round
    tool result
  assistant message    ← final response (text only, no tool_calls)
```

The critical rule: **the assistant message is written before its tools execute.** This means if the editor crashes mid-tool-execution, the file still contains a valid assistant→tool pairing (with the results that completed). On reload, missing results indicate an unclean shutdown — but the file is never structurally malformed.

The editor mirrors this ordering in the transcript: the moment `assistant_item_ready` fires, one `ToolCollapsibleEntry` card is spawned per `tool_call` block in a **pending** state (muted border, animated braille spinner, args-only body). A shared `Timer` in `AIStatusPanel` ticks every 100 ms and advances the spinner glyph on every card currently in `pending_tool_entries`. When the matching `tool_result_ready` fires, the card is looked up by `tool_call_id`, upgraded in place via `update_from_tool_result()` (✓/✗ glyph, status-coloured border, full result body), and removed from the map. When the map empties, the timer stops. Pending state is never persisted — only completed tool items are written to the JSONL file.

On cancellation, any tool calls that weren't dispatched get synthetic `status: "cancelled"` results written before the run exits. This maintains the pairing invariant.

### Read protocol

On editor startup (or chat switch), the JSONL file is read line by line. Each line is parsed and pushed into an in-memory `Vector<HistoryItem>`. The UI rebuilds from this list, and `_build_model_messages()` converts it to OpenAI wire format for the next API call.

### Checkpoints and rewind

The `.meta.json` file stores checkpoint records:

```json
{
  "checkpoints": [
    {
      "checkpoint_id": "ckpt_1775348426898",
      "anchor_ts": 1775348425721,
      "created_at": 1775348426898,
      "item_count": 2,
      "undo_action_index": -1,
      "undo_revert_available": true
    }
  ]
}
```

`anchor_ts` references the user message that triggered the run. On rewind, the JSONL is rewritten (not appended) to contain only items up to the checkpoint's `item_count`. This is the only operation that rewrites the file.

### Raw API log

Alongside each `chat_<id>.jsonl`, a companion `chat_<id>.raw.jsonl` records the raw request and response payloads for every API call the orchestrator makes. This is the file the watchtower dashboard reads to chart per-provider behavior. Each line is a JSON object with:

| Field | Description |
|---|---|
| `ts` | Unix-ms when the entry was written. |
| `chat_id` | Originating chat. |
| `provider`, `model` | Which backend handled the turn (e.g. `gemini` / `gemini-2.5-pro`). |
| `turn` | Monotonic turn number within the run. |
| `direction` | `"request"` or `"response"`. |
| `payload` | The JSON body sent or received. |
| `status_code` | HTTP status (response entries only, omitted on non-errors written without one). |
| `tokens` | Usage dict (response entries only, when the provider reports it). |
| `latency_ms` | End-to-end HTTP round trip — captured from just before `http_client->request()` to just after the full response body arrives. Response entries only, omitted when zero. Also stamped onto the assistant item (`latency_ms` + `provider`) so the editor renders a muted footer (`openai · 1.4s`) under each response bubble. The footer is toggled by the **Debug** button in the chat toolbar (same toggle that shows per-tool token counts). Watchtower reads the journal field to chart per-provider latency. |

`latency_ms` excludes the TCP/TLS connect phase (the persistent connection is already established when `http_client->request()` is called) but includes server-side generation — which is typically the dominant cost and the thing users notice when a provider is slow.

---

## 3. Example Output

All examples are real output from the `a personal vibe` Godot project, formatted for readability (actual files have one JSON object per line, no whitespace).

### 3a. Simple text exchange (no tools)

The most basic case — user sends a message, assistant replies with text only.

```jsonl
{"ts":1775418616465,"item":{
  "role":"user",
  "content":"Testing one two"
}}

{"ts":1775418619164,"item":{
  "role":"assistant",
  "content":[
    {"type":"text","text":"Got it—test received loud and clear. What game dev task can I tackle for you in this Godot project?"}
  ]
}}
```

### 3b. Parallel tool calls (explore phase)

User asks the AI to look at the project. The assistant issues three tool calls in a single response, then summarizes.

```jsonl
{"ts":1775348440430,"item":{
  "role":"user",
  "content":"You bet. Let's take a look at this project."
}}

{"ts":1775348446085,"item":{
  "role":"assistant",
  "content":[
    {"type":"text","text":"Got it—let's explore the project structure.\n\n"},
    {"type":"tool_call","id":"call_68876653","name":"list_open_scenes","args":{}},
    {"type":"tool_call","id":"call_11874741","name":"list_files","args":{"directory":"res://","glob":"*","include_hidden":false}},
    {"type":"tool_call","id":"call_52178676","name":"get_project_settings","args":{}}
  ]
}}

{"ts":1775348446201,"item":{
  "role":"tool","tool_call_id":"call_68876653",
  "content":{
    "status":"success",
    "tool_name":"list_open_scenes",
    "args":{},
    "result":{"count":1,"current_scene":"res://Standard.tscn","scenes":[{"is_current":true,"path":"res://Standard.tscn"}]}
  }
}}

{"ts":1775348445272,"item":{
  "role":"tool","tool_call_id":"call_11874741",
  "content":{
    "status":"success",
    "tool_name":"list_files",
    "args":{"depth":"1 (default)","directory":"res://","glob":null,"include_hidden":false},
    "result":{"objects":["res://Standard.tscn","res://assets/","res://icon.svg","res://icon.svg.import","res://project.godot","res://scripts/"]}
  }
}}

{"ts":1775348446302,"item":{
  "role":"tool","tool_call_id":"call_52178676",
  "content":{
    "status":"success",
    "tool_name":"get_project_settings",
    "args":{},
    "result":{"application/config/name":"a personal vibe","application/run/main_scene":"res://Standard.tscn"}
  }
}}
```

Note: tool results can arrive out of order (list_files at ts `...5272` returned before list_open_scenes at `...6201`). The file preserves actual completion order.

### 3c. Multi-tool batch (scene modification)

User asks to fix background fitting. The AI deletes a Sprite2D, creates a TextureRect, and sets four properties — all in one assistant message.

```jsonl
{"ts":1775419093919,"item":{
  "role":"user",
  "content":"Yep but it's not fitting the screen. Please adjust it so it fits the screen"
}}

{"ts":1775419106974,"item":{
  "role":"assistant",
  "content":[
    {"type":"tool_call","id":"call_30259696","name":"delete_node","args":{"node_path":"Background"}},
    {"type":"tool_call","id":"call_52640436","name":"create_node","args":{"node_name":"Background","node_type":"TextureRect","parent_path":""}},
    {"type":"tool_call","id":"call_06084254","name":"set_property","args":{"node_path":"Background","property_name":"texture","value":"res://japanbackground1.png"}},
    {"type":"tool_call","id":"call_38867012","name":"set_property","args":{"node_path":"Background","property_name":"anchors_preset","value":15.0}},
    {"type":"tool_call","id":"call_13233112","name":"set_property","args":{"node_path":"Background","property_name":"stretch_mode","value":0.0}},
    {"type":"tool_call","id":"call_43710808","name":"set_property","args":{"node_path":"Background","property_name":"z_index","value":-1.0}}
  ]
}}

{"ts":1775419107129,"item":{"role":"tool","tool_call_id":"call_30259696","content":{"status":"success","tool_name":"delete_node","args":{"node_path":"Background"},"result":{"node_name":"Background"}}}}
{"ts":1775419107248,"item":{"role":"tool","tool_call_id":"call_52640436","content":{"status":"success","tool_name":"create_node","args":{"node_name":"Background","node_type":"TextureRect","parent_path":""},"result":{"node_name":"Background"}}}}
{"ts":1775419106396,"item":{"role":"tool","tool_call_id":"call_06084254","content":{"status":"success","tool_name":"set_property","args":{"node_path":"Background","property_name":"texture","value":"res://japanbackground1.png"},"result":{"actual_value":"<Object#null>","target_value":"res://japanbackground1.png"}}}}
{"ts":1775419106760,"item":{"role":"tool","tool_call_id":"call_38867012","content":{"status":"success","tool_name":"set_property","args":{"node_path":"Background","property_name":"anchors_preset","value":15.0},"result":{"actual_value":15,"old_value":0,"target_value":15.0}}}}
{"ts":1775419107274,"item":{"role":"tool","tool_call_id":"call_13233112","content":{"status":"success","tool_name":"set_property","args":{"node_path":"Background","property_name":"stretch_mode","value":0.0},"result":{"actual_value":0,"old_value":0,"target_value":0.0}}}}
{"ts":1775419106975,"item":{"role":"tool","tool_call_id":"call_43710808","content":{"status":"success","tool_name":"set_property","args":{"node_path":"Background","property_name":"z_index","value":-1.0},"result":{"actual_value":-1,"old_value":0,"target_value":-1.0}}}}
```

Six tool calls, six tool results, all paired by `tool_call_id`. The assistant message was written to the file before any of the tools executed.

### 3d. Error results

Tools can fail. The `status` field switches from `"success"` to `"error"`, and `error` replaces `result`:

```jsonl
{"ts":1775419259092,"item":{
  "role":"tool","tool_call_id":"call_07548896",
  "content":{
    "status":"error",
    "tool_name":"run_and_screenshot",
    "args":{"wait_seconds":2.0},
    "error":{"code":"operation_failed","details":{},"message":"Screenshot capture failed: empty image."}
  }
}}

{"ts":1775419269488,"item":{
  "role":"tool","tool_call_id":"call_80675688",
  "content":{
    "status":"error",
    "tool_name":"set_property",
    "args":{"node_path":"Camera2D","property_name":"current","value":true},
    "error":{"code":"invalid_args","details":{},"message":"Property 'current' does not exist on Camera2D. Check the property name."}
  }
}}
```

Errors are persisted with the same structure as successes. The model sees them on the next turn and can adjust its approach.

### 3e. Checkpoint metadata

The `.meta.json` file stores rewind points separately from the conversation stream:

```json
{
  "checkpoints": [
    {
      "checkpoint_id": "ckpt_1775348426898",
      "anchor_ts": 1775348425721,
      "created_at": 1775348426898,
      "item_count": 2,
      "undo_action_index": -1,
      "undo_revert_available": true
    },
    {
      "checkpoint_id": "ckpt_1775348456812",
      "anchor_ts": 1775348440430,
      "created_at": 1775348456812,
      "item_count": 15,
      "undo_action_index": -1,
      "undo_revert_available": true
    }
  ]
}
```

Each checkpoint records how many JSONL items existed at that point (`item_count`). Rewinding truncates the JSONL to that count.

---

## 4. Strengths and Weaknesses

### Strengths

- **Append-only during runs.** No file locking, no read-modify-write cycles during normal operation. The file is opened, one line is written, file is closed. This is crash-resilient — a partial last line is the worst case, and the file up to that point is valid.

- **Human-readable.** JSONL works with `grep`, `jq`, text editors, and version control. You can `grep "tool_name" chat.jsonl` to find every tool call in a conversation, or pipe through `jq` for structured queries.

- **No-orphan invariant.** The write protocol guarantees that every tool_call block has a paired tool result. This means any prefix of the file is a structurally valid conversation that can be sent to the OpenAI API without rejection. Previous versions had a bug where assistant messages with tool_calls were never persisted, causing malformed history on restart.

- **Decoupled from API format.** The canonical format uses content blocks (`text`, `tool_call`) rather than OpenAI's specific `content` + `tool_calls` split. This means switching providers (Claude, Codex) only requires changing the serialization layer, not the storage format.

- **Lightweight.** No database, no schema migrations, no external dependencies. Just files on disk. Each project's chats live in its own `ai_chat/` directory under the Godot user data path.

### Weaknesses

- **No streaming writes.** Each item is written as a complete JSON line after the operation finishes. For long-running tools (like `run_and_screenshot` which can take 18+ seconds), the file doesn't reflect the in-progress state. If the editor crashes during a tool, the tool result is lost (though the assistant message that requested it is preserved).

- **Full rewrite on rewind.** Checkpoint restoration rewrites the entire JSONL file. For long conversations this means writing potentially hundreds of KB. In practice this is fast (< 100ms) but it's architecturally less clean than a WAL or tombstone approach.

- **No compression.** Tool results can be verbose (full script contents, scene tree dumps). A single conversation with heavy `read_script` usage can reach hundreds of KB. There's no deduplication or compression — every tool result is stored in full.

- **Context injections stored but not consumed from file.** `engine_state` and `todo_state` items are written to the JSONL for completeness, but on reload they're ignored — the orchestrator injects fresh context at runtime. This means the stored injections are purely archival right now, not functional.

- **No v1 migration.** Old `.json` chat files from protocol v1 are abandoned. They still exist on disk but aren't loaded. Users lose access to pre-v2 conversation history in the UI (the files are still readable by hand).

- **Windows junction requires cmd.exe.** The centralized folder (`Documents/Godot AI Chats/`) uses `mklink /J` via cmd.exe. This works on all modern Windows without admin, but it's a platform-specific hack. No equivalent is set up for macOS/Linux yet.

---

## 5. Future Work

- **Consume stored injections on reload.** Currently `engine_state` and `todo_state` items in the JSONL are write-only. A future iteration could have the provider adapter read them back from the file when building the API request, rather than relying solely on runtime injection. This would allow the model to see the game state that was active during previous turns, not just the current state.

- **Provider adapter layer.** Right now `_build_model_messages()` in `ai_status_indicator.cpp` does the canonical-to-OpenAI translation inline. This should be extracted into a proper provider adapter that the orchestrator calls, making it straightforward to add Claude or Codex wire formats without touching the UI code.

- **Cross-platform centralized folder.** The Windows junction from `Documents/Godot AI Chats/` works well. macOS (`~/Documents/Godot AI Chats/` with symlinks) and Linux (`~/.local/share/godot-ai-chats/` with symlinks) should get the same treatment.

- **Merge v1/v2/v2.1 spec docs.** The three `conversation_protocol*.md` files track the evolution of the design but are redundant now that v2.1 is implemented. They could be consolidated into a single spec that documents the current system, with a brief "Migration from v1" appendix.

- **Streaming tool progress.** Write a partial tool result line when a tool starts executing, then update it (append a completion line) when it finishes. This would make crash recovery more informative — you'd know which tool was in-flight when the crash happened.

- **Conversation search.** With JSONL files on disk, a lightweight search tool could index conversations by content, tool names, or date ranges. The centralized folder structure already makes this feasible with basic shell scripts.

- **Compression or deduplication.** For conversations with many `read_script` calls on the same file, the full file content is stored each time. A reference-based approach (store the content once, reference by hash) could significantly reduce file sizes for long sessions.
