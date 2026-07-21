# Scene State Diffs

The agent is kept grounded in the file-level truth of scenes it works with: after any tool
batch that mutates a scene, it receives a unified diff of the scene's serialized `.tscn` text,
and at the start of each run it receives diffs for edits made outside the conversation (by the
user in the editor, or external tools like git). This gives the model a single, text-level
source of truth without resending whole scene files.

## Why diffs, and why of the *live* scene

The node tools mutate the live edited scene in memory; the `.tscn` on disk is stale until a
save. A "diff of the scene" therefore serializes the live edited scene (the same pack + text
save the editor's own save performs) and compares that. This captures node-tool edits, user
edits, and file edits uniformly, regardless of disk state.

Diffs complement — not replace — tool results. The tool result is the operation receipt (what
the engine accepted, clamped, or warned about); the diff is the content truth (how it actually
serialized). Notably, `.tscn` files omit properties at their default values, so setting a
property to its default shows up as a line removal or nothing at all; only the tool result
proves the operation happened.

## Snapshots

`scene_diff.cpp` keeps an in-memory, session-scoped map of scene path → the last `.tscn` text
the model has seen. Snapshots are created/refreshed at every point the model "sees" a scene:

- **`read_scene_file` success** — the returned disk text becomes the snapshot.
- **`update_scene_file` success** — the new disk text becomes the snapshot, so the model's own
  file edit is never echoed back to it as a diff.
- **First mutation** — before the first scene-mutating tool call touches a scene with no
  snapshot, a baseline is serialized so the post-batch diff shows exactly that batch's changes.
- **After each emitted diff** — the snapshot advances to the state that was reported.

## Injection points

- **Post-tool-batch (`[SCENE UPDATE]`, attribution "ai")** — after a batch executes, each
  touched scene is re-serialized and diffed against its snapshot. One consolidated message is
  appended to the run's history before the next model turn. Emitted per batch, not per tool
  call.
- **Run start (`[SCENE CHANGES]`, attribution "user")** — every snapshotted scene is checked;
  changes found here happened outside the conversation by definition. Injected before the
  user's current prompt, like the `[GAME SESSION]` block. Scenes no longer open are compared
  via their disk content; deleted scenes are reported as missing and dropped from tracking.
- **Cancelled runs** — a cancelled batch refreshes its snapshots silently (no message), so the
  next run's `[SCENE CHANGES]` pass does not attribute the AI's own changes to the user.

Both messages are ordinary user-role messages appended to (or inserted into) the in-run
conversation history — append-only, so prompt caching is preserved. Like other injections they
are persisted to the chat store (`scene_diff` items) for dashboards, skipped on history
rebuild, and not resent in later runs.

## Size cap

Diffs are line-based unified diffs (Myers, bounded). A diff whose changed-line count exceeds
~300, or whose edit distance exceeds the Myers bound, collapses to a one-line summary telling
the model the scene changed substantially and to use `read_scene_file`. This keeps a scene
rebuild or mass rename from flooding the context.

## Serialization fidelity

Serializing the live scene must produce text that diffs cleanly against editor-saved files:

- A **fresh `PackedScene`** is packed (never the cached resource — repacking the cached one
  would change what new instantiations produce).
- Saved to a **temp file outside `res://`** (never scanned; no uid registered; the saver
  disables subresource path takeover for non-`res://` targets).
- **ext_resource ids** are pre-seeded for the temp path from the on-disk file's id mapping —
  otherwise the saver would generate fresh random ids and every diff would be noise.
- The **header uid** (omitted for non-`res://` saves) is patched back in from the scene's
  registered uid.
- Line endings normalized to LF on both sides of every comparison.

## Known limitations

- Scenes without a file path (brand-new, never-saved tabs) are not tracked.
- User edits made *during* a run land in the post-batch diff and are attributed to the AI's
  tool calls.
- Animation preview state is not reset before packing (the editor's save does this), so a
  snapshot taken mid-preview can include previewed values.
- The chat transcript does not yet render scene-diff bubbles; the data is in the chat store
  for the dashboard.
