---
name: scene-file-editing-autosave-direction
description: update_scene_file refuses on dirty tabs for now; user wants to explore editor-wide autosave (Google Docs model) later
metadata: 
  node_type: memory
  type: project
  originSessionId: d559789c-a121-46f7-9a81-aa966d85991f
---

The `update_scene_file` tool (added July 2026, branch ai-image-stack) **refuses** to edit a .tscn whose editor tab has unsaved changes — no auto-save in v1.

**Why:** User decision (2026-07-18): "lean towards refusing." But he explicitly flagged the future direction: consider auto-saving on every AI action, or editor-wide autosave from user actions too — "like we're working in Google Docs. Saving feels very Microsoft Word."

**How to apply:** If asked to revisit scene-edit ergonomics or reduce save friction, the intended evolution is autosave-by-default (possibly editor-wide), not smarter refusal messages. Conceptual doc: modules/ai/docs/architecture/scene_file_editing.md.
