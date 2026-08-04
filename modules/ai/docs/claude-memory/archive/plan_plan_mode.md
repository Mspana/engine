# Plan: Plan Mode (User-Approved Execution Plans)

## Overview
A user-toggled mode where the AI writes a structured plan (title + milestones) before executing anything. The user can approve, edit (request revisions), or reject the plan. On approval, the AI executes and checks off milestones.

## Trigger
Toggle button in the input bar (flat, `set_toggle_mode(true)`). When ON, `[PLAN MODE REQUEST]` is injected into the conversation before the AI responds. Resets to OFF after a plan is approved.

## New JSON Formats
```json
// PLAN MODE response:
{
  "mode": "plan",
  "title": "5-10 word goal description",
  "assistant_text": "One sentence explaining approach.",
  "milestones": [
    {"id": 1, "task": "What this milestone accomplishes", "done": false},
    {"id": 2, "task": "...", "done": false}
  ]
}

// FINAL MODE during plan execution — add optional plan_update:
{
  "assistant_text": "Done!",
  "actions": [],
  "plan_update": {"completed_milestones": [1, 2, 3]}
}
```

## State Machine
New `RunState` enum values:
- `STATE_PLAN_REQUESTED` — waiting for AI to produce plan
- `STATE_PLAN_REVIEW` — plan received, waiting for user Approve/Edit/Reject
- `STATE_PLAN_EDITING` — user typing revision feedback

```
IDLE (plan toggle ON) → send → STATE_PLAN_REQUESTED
  → AI responds with mode:"plan" → plan_ready signal → STATE_PLAN_REVIEW
    → Approve → inject [PLAN APPROVED] → STATE_RUNNING (normal loop)
    → Edit → STATE_PLAN_EDITING → user sends feedback → STATE_PLAN_REQUESTED (re-plan)
    → Reject → STATE_IDLE, plan cleared
```

## Files to Change

| File | Change |
|---|---|
| `modules/ai/editor/ai_status_indicator.h` | New `RunState` values; `plan_card`, `active_plan_message_id`, `plan_mode_toggle`, `plan_mode_enabled`, `pending_plan`, `pending_plan_prompt` members; new method declarations |
| `modules/ai/editor/ai_status_indicator.cpp` | `_start_plan_run()`; `_on_plan_ready()`; `_on_plan_approve_clicked()`; `_on_plan_edit_clicked()`; `_on_plan_reject_clicked()`; `_start_execution_with_plan()`; `_create_plan_card()`; `_update_plan_card_milestones()`; add plan/plan_update role handling in `_rebuild_message_list()` and `_build_model_messages()`; add plan toggle button in constructor; update `_update_send_button_state()` for new states |
| `modules/ai/agentic_orchestrator.h` | `plan_ready(Dictionary)` signal; `_emit_plan_ready()` method |
| `modules/ai/agentic_orchestrator.cpp` | `_process_model_response()`: detect `mode:"plan"` before validation, emit `plan_ready`, stop run; `_bind_methods()`: register signal |
| `modules/ai/ai_provider.cpp` | Add PLAN MODE format + rules + MILESTONE COMPLETION section to `get_system_prompt()` |

## Chat Store Roles
- `"plan"` — stores full plan JSON (title, milestones array); sent to model as "assistant" role
- `"plan_revision"` — same format, used for AI-revised plans
- `"plan_update"` — `{"completed_milestones": [1, 2, 3]}`; UI-only, skipped in `_build_model_messages()`

## Plan Card Widget Layout
```
PanelContainer [BG_1 bg, BORDER border, or amber-tinted border]
  VBoxContainer
    HBoxContainer [header]
      Label "◈"  [ACCENT_BLUE, 16pt]
      Label <title>  [TEXT_PRIMARY, 14pt]
    Label <assistant_text>  [TEXT_SECONDARY, 13pt, italic]
    HSeparator
    VBoxContainer [milestones]
      HBoxContainer
        CheckBox (read-only)
        Label <task>  [TEXT_PRIMARY / TEXT_MUTED+strikethrough if done]
      ...
    HBoxContainer [action_row — only in STATE_PLAN_REVIEW]
      Button "Approve"  [accent blue]
      Button "Suggest Changes..."  [neutral outline]
      Button "Reject"  [flat, ERROR on hover]
```

## Input Bar States
| State | Prompt | Send button | Placeholder |
|---|---|---|---|
| STATE_PLAN_REQUESTED | Disabled | "Planning..." (disabled) | — |
| STATE_PLAN_REVIEW | Disabled | Disabled | — |
| STATE_PLAN_EDITING | Editable | "Send" | "Describe your changes..." |

## Milestone Completion Tracking
In `_on_orchestrator_complete()`: parse `plan_update.completed_milestones` from final message → call `_update_plan_card_milestones()` → append `"plan_update"` message to chat store → in `_rebuild_message_list()`, apply plan_update messages to the last plan card's checkbox states.

## Edge Cases
- AI responds FINAL MODE during plan phase (simple Q&A) → treat as normal, reset plan toggle
- Rewind into plan flow: if `active_plan_message_id` no longer in transcript → exit STATE_PLAN_REVIEW → STATE_IDLE
- Queued messages during plan review → enqueue (same as STATE_RUNNING)
- Multiple plan revisions: old plan cards lose buttons, become historical read-only cards
