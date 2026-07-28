# AI Module — Design Language

A growing catalogue of motion, color, and component patterns used across the
AI module's editor UI. Each entry below should be reusable by reference, not
re-derived per feature; when a future feature adds animation or styling, look
here first.

---

## Style

1. **Height + fade collapse/expand**
    - When a region is shown or hidden, animate its **height** (`custom_minimum_size:y`, 0 ↔ natural) and its **modulate alpha** (0 ↔ 1) **in parallel** over **0.15 s** with **cubic ease-out** (`TRANS_CUBIC` / `EASE_OUT`).
    - `clip_contents` is set to `true` for the duration of the tween so the body cannot poke out through its parent's edge during the resize. Reset both `clip_contents` and `custom_minimum_size` to their idle values on completion so the layout system can drive natural sizing afterward.
    - Rapid toggles cancel the in-flight tween before starting a new one (hold a `Ref<Tween>` member, kill via a local `Ref` copy because the chained completion callback clears the member).
    - **Suggested for pop-ups** — the same height + fade pattern with a slightly different easing curve reads well for opening/closing modal-style overlays (image viewer, settings dropdowns, command palette).
    - Reference implementation: `ToolCollapsibleEntry::_animate_to_collapsed` and `ThinkingCollapsibleEntry::_on_toggle_pressed` in `modules/ai/editor/ai_status_indicator.cpp`.

---

## Applicable moving areas for style

Places where the patterns above either already apply or should apply when revisited. Tracked here so we don't have to re-discover them later.

### Already using the language

- **Tool result cards** (`ToolCollapsibleEntry`) — height + fade on body expand/collapse.
- **Thinking text** — streamed inline as italic text, slightly lighter than body color (TEXT_SECONDARY), one point smaller. Plain transcript flow, no collapsible (`ThinkingCollapsibleEntry` removed 7/28 in the harness modernization).

### AI module surfaces that should adopt the style

- **`AIImageViewer` popup** — open/close currently uses Godot's default popup show; height + fade would feel consistent with the inline cards.
- **`AIImageStack`** — when a new image set lands, the stack appears instantly; a quick fade-in would soften the arrival.
- **`AITodoPanelWidget`** — when the AI's todo list appears/disappears between runs.
- **`ParseErrorPill`** — expand/collapse of the error list. Already collapsible but no animation.
- **`DebugContextPill`** — expand/collapse of the game-session error list.
- **Pending image thumbnails strip** — appearance when the user attaches an image to a prompt.
- **Model selector dropdown popup** — open/close.
- **History dropdown** — open/close.
- **Error notices** that get inserted into the message list mid-stream.
- **Chat panel itself** — when shown via the toolbar toggle.

### Broader Godot editor surfaces (longer-term, only if/when we touch them)

- **FileSystem dock** — folder tree expand/collapse.
- **Scene tree dock** — node expand/collapse.
- **Inspector** — property category folds, sub-resource expand.
- **Bottom panels** — Output, Debugger, Animation, etc. when toggled.
- **Editor Settings / Project Settings** — section folds.
- **Tab switching** — main screen, scene tabs, inspector tabs.
- **Context menus, tooltips** — appearance fade-in.
- **Standard dialogs** (`FileDialog`, `AcceptDialog`, etc.) — popup show.
- **Editor toaster notifications** — slide / fade in and out.
- **Find/Replace panel** — show/hide.
- **Editor Help** — class doc section folds.

---

Future entries should mirror the format of entry **1**: short headline, the
parameters needed to reproduce, when to reach for it, and a code reference.
