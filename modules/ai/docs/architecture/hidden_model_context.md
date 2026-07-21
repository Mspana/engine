# Hidden Model Context

Context that is sent to the AI model but not directly visible in the chat UI.
This excludes system prompts and tool definitions, which are expected to be hidden.

## Runtime Error and Parse Error Context

Runtime errors and parse errors are injected into the model's conversation as part of the
`[GAME SESSION]` block. Both are stored in the chat history as injection items (`engine_state`
and `parse_error_state`) so they appear in the chat transcript on reload and are available to
the external dashboard.

- **Runtime errors:** Pulled live from the debugger via `get_structured_errors()`. Injected at
  run start. The debug context pill shows error/warning counts and expands to show individual
  errors. An inline collapsible bubble appears in the chat transcript at the point of injection.
- **Parse errors:** Stored on the AI singleton when `update_script` or `create_script` returns
  parse errors. Cleared when the file compiles clean. The parse error pill shows the file and
  error count, and an inline bubble appears in the transcript.
- **Injected in:** `agentic_orchestrator.cpp`, `run_agentic_loop()` via `consume_session_context()`
- **UI visibility:** Full. Both have pills above the input and collapsible bubbles in the chat
  history that persist across chat reloads.

## Retrieval / RAG Context

Semantic search results from the project's retrieval index are injected as a context block
passed to the provider alongside the conversation. Up to 8 file snippets are included, each
with a filename, relevance score, and text excerpt.

- **Injected in:** `ai.cpp` via `provider->send_request()` / `send_request_with_messages()`
- **UI visibility:** None. The user has no indication of which files were retrieved or what
  content the model received as project context.

## User Message Wrapping

Every user message is transformed before sending to the API. The original text is wrapped in
`<user_message>` XML tags, prefixed with a timestamp (`[YYYY-MM-DD HH:MM]`), and an
anti-injection instruction is appended after the closing tag.

- **Injected in:** `ai_status_indicator.cpp`, `_build_model_messages()`
- **UI visibility:** None. The user sees their original message text. The wrapping, timestamp,
  and injection guard are invisible.

## Game Context Screenshot

When the debug context pill is enabled and a game session screenshot exists, it is attached as
a base64 image to the `[GAME SESSION]` context message. This is separate from tool result
screenshots (e.g. from `run_and_screenshot`), which do appear inline in the chat.

- **Injected in:** `agentic_orchestrator.cpp`, `run_agentic_loop()` via `consume_session_context()`
- **UI visibility:** None. The debug context pill shows error/warning counts but does not
  indicate that a screenshot was included.

## TODO State (via update_todos tool results)

Todo state is no longer injected per turn. The former `[CURRENT_TODOS]` per-turn injection was
removed because a fresh trailing message every turn shifted position each turn and invalidated
the prompt-cache prefix (see `prompt_caching.md`). The model's todo state now lives where it
changes: the `update_todos` tool call arguments and the echoed list in its tool result, both of
which are part of ordinary append-only history.

- **UI visibility:** Full. The todo panel shows current state; the tool call and result are in
  the transcript like any other tool use.

## Scene Diff Injection

When AI tool calls change a scene, a `[SCENE UPDATE]` user message with a unified diff of the
scene's serialized `.tscn` text is appended after the tool batch. At run start, a
`[SCENE CHANGES]` message reports scenes edited outside the conversation (user edits) since the
model last saw them. Diffs above a size cap collapse to a "changed substantially" summary. See
`scene_state_diffs.md`.

- **Injected in:** `agentic_orchestrator.cpp`, `_append_batch_scene_diffs()` (post-batch) and
  `run_agentic_loop()` (run start)
- **UI visibility:** Partial. Persisted to the chat store as `scene_diff` injection items
  (visible to external dashboards), but the chat transcript does not yet render a bubble for
  them.

## Timestamp Prefix

A `[YYYY-MM-DD HH:MM]` timestamp is prepended to user message content (part of the XML
wrapping above). This gives the model awareness of when each message was sent.

- **Injected in:** `ai_status_indicator.cpp`, `_build_model_messages()`
- **UI visibility:** None. Message timestamps are not shown in chat bubbles.
