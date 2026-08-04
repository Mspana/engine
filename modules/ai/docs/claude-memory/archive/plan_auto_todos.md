# Plan: Automated AI TODO System (Self-Managed Task Tracking)

## Overview
A new `update_todos` action lets the AI maintain its own internal TODO list during a run. Not user-facing plan mode — the AI's scratchpad. Rendered in a compact panel during active runs. Similar to Claude Code's TodoWrite tool.

## New Action Spec
```json
{
  "action": "update_todos",
  "args": {
    "todos": [
      {"id": "1", "content": "Create player scene", "status": "completed"},
      {"id": "2", "content": "Attach movement script", "status": "in_progress"},
      {"id": "3", "content": "Add collision shape", "status": "pending"}
    ]
  }
}
```
- `todos` is a **full replace** — send the complete list every call
- `status` values: `"pending"` | `"in_progress"` | `"completed"`
- At most one item `"in_progress"` at a time
- Use for tasks with 3+ distinct steps; skip for simple single-step tasks
- Result is silent — suppressed from transcript UI

## Where State Lives
`AgenticOrchestrator::RunContext` — run-scoped, cleared at run start, never persisted to chat store.

```cpp
struct TodoItem {
    String id;
    String content;
    String status; // "pending" | "in_progress" | "completed"
};

struct RunContext {
    // ... existing fields ...
    Vector<TodoItem> todos;
    bool has_todos = false;
};
```

## Context Injection (Every Turn)
In `AgenticOrchestrator::_send_model_request()`, build a **temporary copy** of `conversation_history`, append a synthetic `user`-role `[CURRENT_TODOS]` block when `has_todos == true`, then send. Never mutates `conversation_history`.

```
[CURRENT_TODOS]
[x] 1: Create player scene
[-] 2: Attach movement script
[ ] 3: Add collision shape
[/CURRENT_TODOS]
Update this list using update_todos as you complete steps.
```

## Files to Change

| File | Change |
|---|---|
| `modules/ai/agentic_orchestrator.h` | Add `TodoItem` struct + `todos` / `has_todos` to `RunContext`; add `set_todos()`, `get_todos()` public methods; add `todos_updated(Array)` signal |
| `modules/ai/agentic_orchestrator.cpp` | Implement `set_todos()` (updates state, emits signal); clear todos in `run_agentic_loop()`; inject `[CURRENT_TODOS]` block in `_send_model_request()` |
| `modules/ai/ai.h` | Add `_exec_update_todos()` private method |
| `modules/ai/ai.cpp` | Add `"update_todos"` to `ALLOWED_ACTIONS`; add validation; add dispatch; implement `_exec_update_todos()` |
| `modules/ai/ai_provider.cpp` | Add `update_todos` to action list + behavioral rule 6 (TASK TRACKING) in `get_system_prompt()` |
| `modules/ai/editor/ai_status_indicator.h` | Declare `AITodoPanelWidget` class; add `todo_panel` member and `_on_todos_updated()` handler to `AIStatusPanel` |
| `modules/ai/editor/ai_status_indicator.cpp` | Implement `AITodoPanelWidget`; construct in `AIStatusPanel` constructor (between transcript scroll and separator); connect `todos_updated` signal; implement `_on_todos_updated()`; suppress `update_todos` in `_on_orchestrator_tool_result()`; clear panel in `_on_orchestrator_started()` |

## AITodoPanelWidget Layout
```
PanelContainer [BG_1, BORDER border, CORNER_RADIUS_SM]
  VBoxContainer
    HBoxContainer [header]
      Label "Tasks"  [TEXT_MUTED, 11pt]
      Label "2/4 complete"  [TEXT_MUTED, 11pt, right-aligned]
    VBoxContainer [items]
      HBoxContainer [per item]
        Label status_icon  [✓ SUCCESS / → ACCENT_BLUE / · TEXT_MUTED]
        Label content  [TEXT_PRIMARY or TEXT_DISABLED if completed]
```

## Suppressing from Transcript
In `_on_orchestrator_tool_result()`:
```cpp
if (String(p_tool_result.get("type", "")) == "update_todos") {
    return; // silent — don't render or store
}
```

## Visibility Rules
- Hidden at start of each run (`_on_orchestrator_started()`)
- Shown when `todos_updated` signal fires
- Stays visible after run completes (shows final state, cleared on next run)

## System Prompt Addition
Add to action list:
```
- update_todos: Update your internal task list for this run
  Args: {"todos": [{"id": string, "content": string, "status": "pending"|"in_progress"|"completed"}, ...]}
  Full replace — send the complete current list every call.
  Call at start of multi-step tasks and after each major milestone.
  Do not use for simple single-step tasks.
```

Add behavioral rule (renumber existing 6→7, 7→8):
```
6. TASK TRACKING (multi-step tasks):
   - For tasks requiring 3+ distinct steps, call update_todos early to declare your plan.
   - Update after each major milestone: mark done, advance next to in_progress.
   - Keep items short (5-10 words). IDs are strings: "1", "2", "3".
   - Do not use for trivial one-step tasks.
```

## Interaction with Plan Mode
No conflict — `update_todos` is AI's internal scratchpad (run-scoped, not persisted). Plan mode is a user-facing artifact stored in chat store. Prompt teaches the distinction.
