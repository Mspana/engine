---
name: ai-chat-log-locations
description: "Where to actually find the newest in-engine AI chat logs (real files live in AppData, not Documents)"
metadata: 
  node_type: memory
  type: reference
  originSessionId: 5b1dd813-e6f3-4fb1-b9ab-8db0c0d059aa
  modified: 2026-07-21T04:40:40.312Z
---

"Check the most recent chat" → the real files live under
`C:\Users\Matthew\AppData\Roaming\Godot\app_userdata\<project>\ai_chat\` —
`chat_<ts>.jsonl` (clean transcript), `.raw.jsonl` (full wire log, huge),
`.meta.json`, and `chat_<ts>_images\<call_id>.png` (tool screenshots).
`C:\Users\Matthew\Documents\Godot AI Chats\<project>` entries are Windows
junctions to those dirs; recursive PowerShell listings may not descend into
them, so search AppData directly and sort by LastWriteTime.

As of 2026-07-21 the active test project is **"a personal vibe"**
(`C:\Users\Matthew\Documents\a-personal-vibe`, main scene scene6outside.tscn),
not "test project". Journal logs (runs.jsonl, dev_notes.jsonl) are in the
sibling `ai_journal\` dir per [[MEMORY]] AI Journal Logs note.
