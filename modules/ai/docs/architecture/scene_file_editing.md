# Scene File Editing

The agent can read and edit `.tscn` scene files directly as text, alongside the existing live node tools. Two tools provide this: `read_scene_file` and `update_scene_file`.

## Why text-level editing exists

The node tools (`create_node`, `set_property`, `reparent_node`, ...) operate on the live edited scene through the editor: each change is undoable and validated as it happens. That makes them the right choice for small, interactive tweaks — but building or restructuring a large scene takes one tool call per change, and some things (embedded sub_resources like shapes and gradients, mass renames, repointing many resource references) are awkward or impossible to express through them.

Scene files are plain text, so a single string-replacement edit can do bulk or structural work in one step. The trade-off: file edits bypass the editor's undo system. The system prompt steers the model accordingly — node tools for small tweaks, file edits for bulk work.

## Tool surface

- **`read_scene_file(file_path)`** — returns the raw `.tscn` text from disk, plus `is_open_in_editor` and `has_unsaved_changes` flags. When the scene's tab has unsaved changes, the result carries a warning that the disk content is stale.
- **`update_scene_file(file_path, old_string, new_string, replace_all=false)`** — exact string replacement, mirroring `update_script`: the edit fails if `old_string` is missing or ambiguous. Requires a prior `read_scene_file` of the same file in the conversation (enforced by scanning conversation history, same mechanism as the script tools).

Both are restricted to `res://` paths with a `.tscn` extension.

## Safety model

The write path refuses rather than guessing:

- **Unsaved editor changes** — if the scene is open in a tab with unsaved changes, the edit is refused. Writing the file and reloading would silently discard the user's work; the user (or the agent, via `save_scene` on the active scene) must save first. There is deliberately no auto-save in v1.
- **Running game** — edits are refused while the game is playing.
- **Scene uid** — an edit that would change or remove the `uid` in the `[gd_scene]` header is rejected; other files reference the scene through that uid. `ext_resource` uids are *not* guarded — repointing a reference legitimately involves updating them.
- **Missing references** — every `ext_resource` path in the edited content is checked for existence before committing. This is a separate check because the editor loads with missing-resource errors downgraded, so the validation load alone would not catch a broken path.
- **uid-beats-path warning** — at load time an `ext_resource`'s registered uid takes precedence over its `path` attribute. If an edit leaves a uid pointing at a different file than the path names, the tool warns that the uid will win.

## Validation pipeline

Before the real file is touched, the edited content is written to a temp file outside the project (the editor's temp directory, which the filesystem scanner never sees and whose uid header is not registered by loading) and loaded as a `PackedScene` with caching disabled. If the load fails, the tool returns the actual parse errors — captured from the engine's error stream, including line numbers — and the file on disk is left untouched. The temp file is always deleted before returning.

## Editor synchronization

After a successful commit the tool tells the editor's filesystem cache about the change, and — only if the scene is open in a tab — reloads that tab from disk in place. The reload preserves tab order and the current tab, and updates the editor's stored modification time so the "files have been modified outside Godot" dialog does not appear afterward. Reloading clears that tab's undo history, which is why file edits are not undoable.

Line endings are normalized to LF on edit (matching what the editor's own save produces), so files checked out with CRLF are silently converted the first time they are edited.

## Interaction with scene state diffs

Both tools feed the scene snapshot system (`scene_state_diffs.md`): a successful
`read_scene_file` makes the returned text the model's tracked baseline for that scene, and a
successful `update_scene_file` advances the baseline to the new disk content so the model's own
edit is not echoed back to it as a `[SCENE UPDATE]` diff.

## Known limitations / future work

- Not undoable with Ctrl+Z (by design; the tab reload clears undo history).
- `.tres` text resources are not yet supported — scenes only.
- If the edited scene is instanced inside *other* open scenes, those embedded instances are not refreshed; they update when their own scene is reloaded or reopened.
- No auto-save-before-edit; a broader editor autosave model may revisit this.
