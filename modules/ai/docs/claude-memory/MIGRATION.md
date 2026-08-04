# Claude Code Memory Migration

This folder is a snapshot of the Claude Code persistent memory for this project, taken
2026-08-04 from the machine where the project lived at `c:\Users\Matthew\Documents\engine`.
It exists so the memories can be restored on a new PC. It is **not** read by Claude Code
from here — memories only work from the per-project memory directory under `~\.claude`.

## Instructions for the AI performing the migration

1. Determine this project's memory directory on this machine. Claude Code derives it from
   the project's absolute path: `~\.claude\projects\<slug>\memory\`, where `<slug>` is the
   project path with separators replaced by dashes (e.g. `c:\Users\Matthew\Documents\engine`
   → `c--Users-Matthew-Documents-engine`). Your system prompt's Memory section states the
   exact directory — use that.

2. Copy every `.md` file in this folder **except this file (MIGRATION.md)** into that
   memory directory, preserving the `archive\` subfolder. Do not overwrite a memory file
   that already exists at the destination with newer content — merge by hand if both exist.

3. `MEMORY.md` is the index that gets loaded into context each session. Verify it lists
   the migrated files and contains no content that only made sense on the old machine.

4. Fix machine-specific facts. These memories reference paths from the old PC — verify
   and update them for this machine (or delete the memory if it no longer applies):
   - `ai-chat-log-locations.md` — AppData chat log paths, junction layout, active test project
   - `gibdulbasit-pages-deploy.md` — location of the `gibdulbasit` repo clone
   - `cursor-workspace-setup.md` — path of `engine.code-workspace`
   - `MEMORY.md` — AI journal log paths under AppData

5. Timestamps: memory freshness is judged by file modification time. Copying resets it;
   that is acceptable — treat content, not mtime, as the source of truth. The snapshot
   date at the top of this file is the true "as of" date for all of them.

6. When done, tell the user which memories were migrated, which were edited for the new
   machine, and which were dropped. This folder can then be deleted from the working tree
   if the user wants (it stays in git history).

## Snapshot contents

Active memories: `MEMORY.md` (index), `harness-ui-parity-target.md`,
`cursor-workspace-setup.md`, `feedback_builds.md`, `feedback_feature_testing.md`,
`scene-file-editing-autosave-direction.md`, `ai-chat-log-locations.md`,
`gibdulbasit-pages-deploy.md`, `backlog.md`, `plan_compact_conversation.md`.

Archive (implemented plans, kept for history): `archive\plan_plan_mode.md`,
`archive\plan_narration_mode.md`, `archive\plan_auto_todos.md`.
