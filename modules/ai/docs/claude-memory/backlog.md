# Aristotle AI Module — Backlog

## Recently Completed
- ✅ Automated AI TODO system (`update_todos` action + `AITodoPanelWidget`)
- ✅ Context usage % indicator in status bar
- ✅ Dynamic context budget per model (`get_context_window_tokens`)
- ✅ Refresh context usage on startup (not just after first send)
- ✅ Narration stored as JSON in conversation history (prevents re-narration)
- ✅ System prompt rule: never narrate more than once per request
- ✅ Validation error repair message with concrete JSON templates
- ✅ Show "Model attempted:" content in validation error UI (collapsed body)
- ✅ JSON mode enforcement: `response_format: json_object` (OpenAI/xAI), `responseMimeType` (Gemini)
- ✅ Markdown → BBCode conversion for chat bubble rendering
- ✅ CLAUDE.md: never commit without manual testing

## Tabled (plans saved)
- 📋 **Compact/summarize conversation** — full plan in `plan_compact_conversation.md`

## Pending
- 🔲 **External system prompt file** — load from `res://` or `user://` instead of hardcoded C++ string; enables prompt iteration without recompile
- 🔲 **Prompt caching** — OpenAI/xAI already cache automatically; Gemini explicit caching (90% discount) requires cache creation + reference per request; optional routing headers for OpenAI (`prompt_cache_key`) and xAI (`x-grok-conv-id`) improve hit rates
- 🔲 **Hallucination on repair** — auto-repair now silently injects `"actions": []` when `assistant_text` is present but `actions` missing (handles done-but-forgot-empty-array); true mid-run hallucination (claiming to have done work without executing actions) remains unverified
- 🔲 **AI can stop the game** — new action `stop_game` that requests the editor stop the running game; show a confirmation popup "The AI would like to stop the game so it can make changes. Stop the game?" with Yes / No / Always quit without asking buttons; once implemented, `update_script` / `write_script` should auto-invoke `stop_game` when they get `operation_failed: game is running` instead of asking the user to stop manually

## Tool Result UX
- 🔲 **Inline file diff in chat** — when `write_script` / `update_script` / file-write actions complete, show a collapsed git-diff-style view (added lines green, removed lines red) inside the tool result bubble; clicking the filename opens that file in the editor
- 🔲 **Property change diff in chat** — when `set_property` (or similar) completes, show old value → new value inline; clicking navigates to the node in the scene tree and highlights the property in the Inspector (likely needs a helper that selects the node + focuses the Inspector dock)

## UI Polish
- 🔲 **Animated thinking indicator** — replace static "Assistant is thinking..." pending bubble with animated `thinking.` → `thinking..` → `thinking...` → repeat (timer-driven dot cycle)
- 🔲 **Chat message style pass** — general visual improvements to message bubbles, spacing, typography, colors across all message types (user, assistant, narration, tool, thinking, error)

## Ideas / Future
- 🔲 **Vision / image input** — pass images to the model alongside text; foundation for all screenshot workflows
- 🔲 **Game screenshot capture** — run the game for N seconds, capture frames every X ms via Godot's `get_viewport().get_texture()`, encode as PNG/base64, feed to the model; lets the AI visually inspect gameplay and iterate without the user describing what they see


- 🔲 **Plan mode** — user-toggled mode where AI writes a plan for approval before executing (plan in archive/plan_plan_mode.md)
- 🔲 **Structured output schema** — use `response_format: {type: "json_schema", schema: {...}}` (OpenAI) to enforce `assistant_text` + `actions` shape at API level, not just syntactic JSON
