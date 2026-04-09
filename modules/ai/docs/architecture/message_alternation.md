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
