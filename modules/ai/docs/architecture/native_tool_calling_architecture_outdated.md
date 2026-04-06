# Native Tool-Calling Architecture

## Overview

The AI module uses **native API tool-calling** (OpenAI-compatible function calling) instead of a custom JSON response format. The API structures tool calls, text output, and `finish_reason` natively — no `response_format: json_object`, no JSON validation/repair cycles, no action schema in the system prompt.

The model receives tool definitions via the `tools` array in the API request and responds with `tool_calls` when it wants to act. The orchestrator executes each tool, returns results as `role: "tool"` messages, and loops until the model responds with `finish_reason: "stop"`.

## Data Flow

```
User types message in chat
        │
        ▼
_build_model_messages()          ◄── reads ChatStore, truncates to context window,
        │                            wraps user messages in <user_message> tags
        ▼
run_agentic_loop(messages, provider)
        │
        ▼
_send_model_request()            ◄── guardrail checks (turns, actions, cancellation)
        │                            injects TODO state if active
        ▼
provider->send_request_with_messages()   [async, worker thread]
        │
        ├─ build_request_body_with_messages()
        │   ├─ system message (behavioral prompt)
        │   ├─ conversation messages (preserving tool_calls & tool results)
        │   ├─ tools: build_tools_array()  (30 tool definitions)
        │   └─ tool_choice: "auto"
        │
        └─ POST to api.x.ai/v1/chat/completions
                │
                ▼
_on_provider_response()          [main thread, signal callback]
        │
        ├─ parse full JSON response
        └─ call_deferred → _process_model_response_deferred()
                │
                ▼
_process_native_tool_response()  ◄── the core loop
        │
        ├─ extract finish_reason, content, tool_calls from choices[0]
        ├─ append assistant message to conversation_history
        │   (preserving tool_calls array for API roundtrip)
        │
        ├─ if content present & not final turn:
        │   └─ emit narration_ready (UI shows model's reasoning)
        │
        ├─ if finish_reason == "stop":
        │   └─ emit run_complete → done
        │
        └─ if tool_calls present:
            └─ for each tool call:
                ├─ parse function.name + function.arguments
                ├─ [special] run_and_screenshot → async state machine, return
                └─ [normal] _execute_tool_call()
                    ├─ AI::execute_single_action(action)
                    ├─ build {role: "tool", tool_call_id, content: result_json}
                    ├─ append to conversation_history
                    └─ emit tool_result_ready (UI shows tool result)
            │
            └─ _send_model_request()  ──► loop back
```

## Key Components

### 1. Tool Definitions — `build_tools_array()`

`ai_provider.cpp` — Returns an Array of 30 tool definitions in OpenAI function-calling format:

```json
{
  "type": "function",
  "function": {
    "name": "create_node",
    "description": "Create a new node in the scene tree",
    "parameters": {
      "type": "object",
      "properties": { ... },
      "required": ["node_name", "node_type"]
    }
  }
}
```

**Tools by category:**

| Category | Tools |
|----------|-------|
| Nodes | `create_node`, `delete_node`, `duplicate_node`, `set_property`, `create_resource`, `rename_node`, `reparent_node` |
| Scripts | `create_script`, `update_script`, `attach_script`, `detach_script`, `rename_script`, `delete_script` |
| Signals | `connect_signal`, `disconnect_signal` |
| Scenes | `create_scene`, `open_scene`, `save_scene`, `close_scene`, `set_main_scene` |
| Project | `set_project_setting`, `get_project_settings`, `create_autoload_singleton`, `remove_autoload_singleton`, `import_asset`, `delete_asset` |
| Run/Test | `run_project`, `run_and_screenshot` |
| Inspect | `list_nodes`, `get_node_info`, `find_nodes_by_type`, `list_files`, `read_script` |
| Meta | `write_dev_note`, `update_todos` |

Helper functions `_make_prop(type, desc)` and `_make_tool(name, desc, props, required)` keep definitions concise.

### 2. System Prompt — `get_system_prompt_native_tools()`

Behavioral guidance only (no action schemas — those live in the tools array). Contains:

- **Identity**: "Aristotle, a Godot 4 AI assistant built into the game engine"
- **Turn structure**: Plan → Execute → Verify → Done
- **Premature completion rules**: Never claim done without verifying via tool results
- **Behavioral rules**: Reason before acting, read tool results, fix parse errors immediately
- **Godot best practices**: Prefer scene/node structure over GDScript where possible
- **Node path rules**: Always relative to scene root
- **Diagnostics**: How to interpret warnings arrays in tool results

### 3. Request Assembly — `XAIProvider::build_request_body_with_messages()`

Constructs the API request body with:

- `model`, `temperature`, `max_tokens`
- `tools`: from `build_tools_array()`
- `tool_choice`: `"auto"`
- `messages`: system message + conversation history

Message types handled:
- `role: "tool"` with `tool_call_id` → passed through for API roundtrip
- `role: "assistant"` with `tool_calls` array → preserved exactly
- `role: "user"` → plain text or multipart (with images for vision models)

### 4. Orchestrator Loop — `_process_native_tool_response()`

The core decision point after each API response:

1. **Extract** `finish_reason`, `content`, `tool_calls` from `choices[0].message`
2. **Store** assistant message in conversation history (with `tool_calls` intact)
3. **Emit narration** if content exists and model isn't done yet
4. **If `finish_reason == "stop"`** → run complete, emit final message
5. **If `tool_calls` present** → execute each, store results, loop back

### 5. Tool Execution — `_execute_tool_call()`

1. Build action dict: `{action: tool_name, args: parsed_args}`
2. Call `AI::execute_single_action(action)` → returns `{status, result/error, ...}`
3. Build API message: `{role: "tool", tool_call_id: id, content: JSON(result)}`
4. Build UI display data: `{tool_name, action_id, type, args, status, result}`

### 6. Async `run_and_screenshot`

Special-cased because it requires the game to run and render before capturing. Uses a timer-based state machine:

```
POLL_START (wait for game to launch, up to 8s)
    → WAIT_VISUAL (wait N seconds for visual stability)
        → AWAIT_CAPTURE (trigger screenshot, wait for callback)
            → _on_async_rns_complete() → resume loop
```

Uses a generation counter (`_rns_tick_gen`) to discard stale timer callbacks.

## Guardrails

| Limit | Default | Checked in |
|-------|---------|------------|
| `MAX_MODEL_TURNS_PER_RUN` | 12 | `_send_model_request()` |
| `MAX_ACTIONS_PER_RESPONSE` | 12 | `_process_native_tool_response()` |
| `MAX_ACTIONS_PER_RUN` | 25 | `_send_model_request()` |

Cancellation is checked at every checkpoint: before sending, on response, during tool loop, during async completion.

## Cross-Run History

Between runs, `_build_model_messages()` rebuilds conversation from `ChatStore` as plain `user`/`assistant` pairs. The API only requires `tool_call_id` pairing within a single assistant→tool sequence — historical context works fine as simple messages.

Context is truncated to fit the model's context window (reserves 10k tokens for system prompt + output).

## Message Format Reference

**Assistant message with tool calls (stored in history):**
```json
{
  "role": "assistant",
  "content": "I'll create that node for you.",
  "tool_calls": [
    {
      "id": "call_abc123",
      "type": "function",
      "function": {
        "name": "create_node",
        "arguments": "{\"node_name\": \"Player\", \"node_type\": \"CharacterBody2D\"}"
      }
    }
  ]
}
```

**Tool result message (stored in history):**
```json
{
  "role": "tool",
  "tool_call_id": "call_abc123",
  "content": "{\"status\": \"success\", \"result\": \"Created node 'Player' (CharacterBody2D)\"}"
}
```

**Final assistant message (finish_reason: "stop"):**
```json
{
  "role": "assistant",
  "content": "Done! I've created a CharacterBody2D node called Player."
}
```
