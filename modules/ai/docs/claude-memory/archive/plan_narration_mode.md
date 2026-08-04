# Plan: Initial Reasoning / Narration Response

## Overview
Add a mandatory first-turn narration step before the AI takes any actions. The AI describes what the user wants and its intended approach in plain text, then continues into the normal action loop.

## New JSON Format
```json
// NARRATION MODE (turn 1 only):
{"mode": "narration", "assistant_text": "2-4 sentences: restate goal, what you need to explore, intended approach."}

// Existing ACTION MODE gains explicit mode field:
{"mode": "action", "assistant_text": "...", "actions": [...]}

// Existing FINAL MODE gains explicit mode field:
{"mode": "final", "assistant_text": "...", "actions": []}
```

## Behavioral Rules
- Turn 1 MUST be NARRATION MODE (unless it's a simple Q&A → may skip to FINAL)
- After NARRATION, loop continues immediately (no user input needed)
- Cannot send NARRATION MODE on turn 2+ (orchestrator rejects it)

## Files to Change

| File | Change |
|---|---|
| `modules/ai/ai_provider.cpp` | Add NARRATION MODE to `get_system_prompt()` format spec + behavioral rule 0 |
| `modules/ai/agentic_orchestrator.h` | Add `narration_ready` signal; `expecting_narration` bool in `RunContext`; `_emit_narration()` helper |
| `modules/ai/agentic_orchestrator.cpp` | `_validate_response()`: accept narration mode; `_is_final_response()`: guard narration; `_process_model_response()`: new narration branch; append narration to conversation_history as "assistant" |
| `modules/ai/editor/ai_status_indicator.h` | Declare `_on_orchestrator_narration()`, `_create_narration_bubble()`, `_append_narration_ui()` |
| `modules/ai/editor/ai_status_indicator.cpp` | Map `"narration"` → `"assistant"` in `_build_model_messages()`; add narration case to `_rebuild_message_list()`; implement rendering (non-collapsible card with left accent stripe, TEXT_SECONDARY, "Planning..." header); connect `narration_ready` signal |

## UI Design
- Non-collapsible (always expanded)
- `ASSISTANT_BG` background + 3px left border in `ACCENT_BLUE_MUTED`
- Small italic "Planning..." label above the text
- Text in `TEXT_SECONDARY`
- Sits before any tool result collapsibles for that turn

## Orchestrator Flow
```
Turn 1: NARRATION MODE → emit narration_ready → append to history as "assistant" → _send_model_request()
Turn 2..N: ACTION MODE → progress_update (thinking collapsibles) → execute → loop
Turn N+1: FINAL MODE → run_complete → _on_orchestrator_complete()
```

## Edge Cases
- Pure Q&A (no actions): AI may skip NARRATION and go directly to FINAL — allowed
- `expecting_narration = true` + model sends ACTION MODE → validation error: must narrate first
- Narration counts against MAX_MODEL_TURNS (currently 12 → bump to 14 if needed)
- Old transcripts without narration role: fall through to default `_create_message_bubble()` — safe degradation

## State Storage
- `RunContext.expecting_narration = true` at run start, flipped to `false` after first narration received
- Narration stored in chat store with role `"narration"`, persisted normally
