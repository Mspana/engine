# Plan: Compact Conversation

Summarizes the conversation history to free up context window space, similar to Claude Code's compaction. Client-side only — requires an LLM API call.

---

## Architecture

**Owner:** `AIStatusPanel` (not `AgenticOrchestrator`). Compaction is a one-shot plain-text request, not an agentic loop. Keeping it in the panel keeps it close to the store and the UI.

**Key blocker:** `send_request_with_messages()` always prepends the JSON action system prompt. Compaction must NOT use that prompt — it needs the model to respond in plain text. Solution: add a new virtual method to `AIProvider`:

```cpp
virtual void send_plain_request(const Array &p_messages);
```

`p_messages` is fully self-contained, including a custom system message at index 0. Implementations skip `get_system_prompt()` injection. OpenAI and xAI are near-identical; Gemini needs special handling (no native system role — prepend system text to first user message, same pattern as existing `GeminiProvider::build_request_body_with_messages`).

---

## New Members (ai_status_indicator.h)

```cpp
Button *compact_button = nullptr;
bool _is_compacting = false;
```

New methods:
```cpp
void _on_compact_pressed();
void _on_compact_response(bool p_success, const String &p_response, const String &p_error);
Control *_create_summary_bubble(const String &p_summary_text);
```

---

## Flow

1. User clicks **Compact** button
2. `_on_compact_pressed()`:
   - Guard: disabled if `run_state != STATE_IDLE` or `_is_compacting`
   - Set `_is_compacting = true`, update button text to "Compacting..."
   - Build message array: `[system_msg] + _build_model_messages() + [user_compaction_prompt]`
   - Connect `request_completed` signal → `_on_compact_response`
   - Call `provider->send_plain_request(messages)`
3. `_on_compact_response()`:
   - Always disconnect signal first
   - **On success:** `chat_store->clear_transcript()` → `chat_store->append_message("user", "[CONVERSATION SUMMARY]\n" + summary)` → `_rebuild_message_list()` → `_refresh_context_usage()`
   - **On failure:** re-enable button, show error assistant bubble in chat
   - Set `_is_compacting = false`, restore button text

---

## UI

**Button placement:** In the `button_column` VBoxContainer below `clear_button`. Same neutral style.

**Disable rules** (add to `_update_send_button_state()`):
- Disabled when `run_state != STATE_IDLE`
- Disabled when `_is_compacting`
- Disabled when transcript has fewer than 4 messages

**Summary bubble:** New `_create_summary_bubble()` — same style as narration bubble but with an amber/muted-yellow left stripe instead of blue, and "Conversation summarized" as the header. Detected in `_rebuild_message_list()` by `msg.content.begins_with("[CONVERSATION SUMMARY]")`.

---

## Compaction Prompt

### System message (role: "system"):
```
You are a conversation summarizer. Your task is to produce a structured,
lossless-as-possible summary of the AI assistant session provided.
You are NOT Aristotle and you do NOT respond in JSON action format.
Respond only with a plain-text summary — no JSON, no code blocks wrapping the summary itself.

Your summary must preserve:
- The user's original goals and all significant sub-tasks requested
- Every file that was created, modified, or deleted (with file paths)
- Every GDScript or resource change made, described at function/property level
- Scene tree changes: nodes added, removed, renamed, or reparented
- Any errors encountered and how they were resolved
- The current state of the project as of the end of the conversation
- Any explicit decisions or preferences stated by the user
- Any tasks that were explicitly left incomplete or deferred

Be thorough. This summary will replace the entire conversation history.
```

### User message (appended after full history):
```
Please summarize the conversation above. Structure the summary as follows:

## Goals
[What the user was trying to accomplish]

## Files Changed
[List each file with a one-line description of what changed]

## Current Project State
[Scene structure, active scripts, any known issues]

## Decisions & Preferences
[Any explicit user decisions, preferences, or constraints]

## Incomplete / Deferred Tasks
[Anything started but not finished, or explicitly left for later]
```

If `extra_instructions` is provided (future feature), append: `\n\nAdditional focus: <extra_instructions>`

---

## Files to Change

| File | Change |
|------|--------|
| `ai_provider.h` | Add `virtual void send_plain_request(const Array &p_messages)` to base + all 4 providers |
| `ai_provider.cpp` | Implement `send_plain_request` in OpenAI, Gemini, XAI, Dummy — skips system prompt injection |
| `ai_status_indicator.h` | Add `compact_button`, `_is_compacting`, and 3 new method declarations |
| `ai_status_indicator.cpp` | Button construction, `_on_compact_pressed`, `_on_compact_response`, `_create_summary_bubble`, `_rebuild_message_list` case, `_update_send_button_state` update, `_bind_methods` registration |

---

## Open Questions / Risks

1. **Truncation during compact itself** — `_build_model_messages()` applies the char budget. If the conversation already exceeds the budget, the summary won't cover the oldest messages. Options: (a) bypass truncation for compact and send raw store contents up to the full model window, or (b) warn the user that early context is already lost.

2. **Signal conflict** — `request_completed` is shared with the agentic orchestrator. Since compact is disabled during runs this can't race, but `_on_compact_response` must connect before the call and disconnect immediately in both success and failure paths.

3. **Gemini system role** — Gemini doesn't support `"system"` role natively. `send_plain_request` for Gemini must prepend the system message text to the first user message.

4. **Token cost** — A full compact on a large session is a non-trivial API call. Consider adding a tooltip note on the button: "Uses API tokens proportional to conversation length."

5. **Checkpoints destroyed** — `clear_transcript()` wipes all `ChatCheckpoint` records. Rewind buttons on prior messages disappear with the messages, so this is correct behavior — no special handling needed.
