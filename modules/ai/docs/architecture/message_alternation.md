# Message Alternation and Provider Error Recovery

## Problem

Anthropic's Messages API requires strict user/assistant role alternation. Sending two consecutive messages with the same role returns HTTP 400. OpenAI and xAI APIs do not have this restriction.

Our system can produce consecutive `user` messages in practice:

1. User sends a message -- appended to `chat_store` immediately
2. API call fails (429 rate limit, 500, network error, etc.) -- error stored as `role: "system"` (skipped during message building)
3. No assistant response is stored
4. User sends another message -- a second `user` item is appended
5. `_build_model_messages()` produces `[..., user, user, ...]`
6. Anthropic rejects this with 400; OpenAI/xAI accept it fine

## How Claw Avoids This

Claw (an agentic coding tool, competitor to Claude Code) uses the Anthropic API and does NOT enforce alternation in its message-building code. It avoids the problem architecturally:

**Temporary runtime pattern**: Each user turn runs inside a temporary copy of the session. The user message is appended to this copy before the API call. If the call fails, the temporary copy is discarded -- `replace_runtime()` and `persist_session()` are only called on success. The real session state never sees the orphaned user message.

```
// Claw's REPL (simplified)
fn run_turn(&mut self, input: &str) {
    let mut runtime = self.prepare_turn_runtime();  // clone
    runtime.push_user_message(input);                // append to clone
    let result = runtime.run_turn();
    match result {
        Ok(_) => self.replace_runtime(runtime),      // commit on success only
        Err(_) => { /* discard runtime, session unchanged */ }
    }
}
```

**Retry/fallback layer**: Claw also retries 429/5xx errors with exponential backoff + jitter at the HTTP layer, and supports provider fallback chains. Most transient errors never surface as failed turns.

**On-disk healing**: Even though Claw's `push_message` incrementally appends to a JSONL file during a turn, a later successful turn calls `save_to_path` which writes a full snapshot, overwriting any stray appended records.

## How Codex Handles This

Codex uses the OpenAI API, which accepts consecutive same-role messages. The problem doesn't arise.

Codex does persist errors as first-class items (`ErrorItem` type), but since OpenAI doesn't enforce alternation, orphaned user messages before error items are harmless.

## Our Architecture

We append the user message to `chat_store` eagerly (before the API call), so it appears in the UI immediately. This is intentional -- if the app crashes mid-request, the user's message is preserved.

Error notices are stored as `{role: "system", type: "error"}` items. The `_build_model_messages()` function skips `system` role items, so errors are never sent to the API. But this also means a failed run leaves `[..., user]` with no corresponding assistant response.

## Chosen Fix: Provider-Side Merge (Option 3)

We merge consecutive same-role messages inside `AnthropicProvider::build_request_body_with_messages`, after building the translated message array but before assigning it to the request body. This is the most isolated fix -- it doesn't change shared message-building logic or chat store persistence, and only affects the provider that requires alternation.

### Alternatives Considered

**Option 1 -- Claw's approach (don't persist until success):** Would require refactoring chat store and UI to defer user message display. Risk: losing the user's message on crash. Big change for a narrow problem.

**Option 2 -- Roll back on failure:** Remove the orphaned user message from `chat_store` when a run fails without producing an assistant response. Cleaner data, but adds complexity to the failure path and could lose messages if the error handling itself fails.

**Option 3 -- Merge in provider (chosen):** Merge consecutive same-role messages in the Anthropic provider's request builder. Doesn't fix the underlying data shape but prevents the 400. Simplest, most isolated, provider-specific.

## Dangling Tool Calls (Missing Tool Results)

A related strictness: OpenAI-compatible APIs reject a request if an assistant message advertises `tool_calls` that are not each answered by a `role: "tool"` message before the next non-tool message (Moonshot enforces this; Anthropic has the same rule for `tool_use`/`tool_result` blocks).

Rebuilt histories used to violate this: `update_todos` results were suppressed from the transcript with an early return that also (unintentionally) skipped chat-store persistence. Live runs were fine — the orchestrator's in-memory history had the results — but any follow-up message rebuilt history from the store via `_build_model_messages()`, producing unanswered `update_todos` tool_calls, and the whole request failed with HTTP 400. A crash mid-run can leave the same shape (assistant item persisted, its tool results never written).

Two-part fix, both in `ai_status_indicator.cpp`:

1. **Persist everything:** `_on_orchestrator_tool_result` now suppresses only the transcript card (and token count) for `update_todos`; the result is persisted to the store like any other tool. The reload path filters the same items from display, so live and reloaded transcripts match.
2. **Repair on rebuild:** `_build_model_messages()` runs a repair pass that synthesizes a stub tool response (`status: "unknown"`, with an explanatory note) for any tool call left unanswered, so transcripts saved before the fix — and crash-truncated ones — remain usable.

## Empty Assistant Messages

Another strictness in the same family: Moonshot rejects any request containing an assistant message with no content and no tool calls (`HTTP 400: the message at position N with role 'assistant' must not be empty`).

Reasoning models can produce exactly that shape. Kimi's thinking channel counts against the completion budget, so a hard problem can consume the entire `max_tokens` on `reasoning_content` and return `finish_reason: "length"` with empty `content` and no tool calls. The orchestrator used to treat this as a successful final answer and persist an empty assistant item — after which every follow-up message replayed it and the whole chat was permanently rejected with 400.

Two-part fix, mirroring the dangling-tool-call approach:

1. **Don't persist it:** `_process_native_tool_response` (orchestrator) now ends the run with a visible error when a response has no content and no tool calls — "model ran out of output tokens while reasoning" for `finish_reason: "length"` — instead of storing an empty assistant item. The error is persisted as a `system`/`error` item, which message building already skips.
2. **Repair on rebuild:** `_build_model_messages()` skips any assistant item that yields no text and no tool calls, so transcripts saved before the fix remain usable. Skipping can leave consecutive `user` messages, which the Anthropic provider's same-role merge (above) already handles.

Additionally, `MoonshotProvider` raises `max_tokens` from the global 8000 default to 32768, since always-on reasoning shares that budget and 8000 is easily exhausted before any visible output is produced.
