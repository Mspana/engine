---
name: Feature testing feedback (screenshot, borders, property monitor)
description: User test results and design direction for three features built in worktrees
type: feedback
---

**Screenshot focus fix** — Made things WORSE. The fix (`window_move_to_foreground` + `grab_focus` before deferred capture) closes the running test before taking the screenshot. Branch `feature/screenshot-focus` is rejected; needs a fundamentally different approach.

**Why:** Grabbing focus or moving window to foreground apparently triggers a stop/defocus of the embedded game process.

**How to apply:** Do not merge screenshot-focus. Investigate why focus change kills the game before attempting a new fix.

---

**Green/red tool result borders** — Approved and useful. Keep borders.

**I/O debug button** — Removed. Not useful as-is; user plans bigger rework of the tool call UI entirely.

**How to apply:** Never add raw transcript viewers to the panel without explicit request. Borders-only approach is the right scope for now.

---

**watch_game_properties / Monitored Play** — The tool failed in testing. The feature concept is approved but needs major redesign:
- Rename: "monitored play" not "watch_game_properties"
- Watch full entity list or filtered list (regex by name, type, properties, subproperties)
- Support late-spawning entities: "when entity X spawns, monitor property Y"
- Current implementation is too narrow (requires exact node paths upfront)

**Why:** Current implementation writes a static GDScript autoload that only monitors pre-specified paths. Doesn't handle dynamic scenes.

**How to apply:** Don't merge feature/property-monitor. Treat as a prototype/spike. Full redesign needed before building again.
