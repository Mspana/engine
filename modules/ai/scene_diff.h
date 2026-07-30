// modules/ai/scene_diff.h
// Scene snapshot + diff support for the AI module.
//
// Tracks the last .tscn text the model has seen per scene ("snapshot") and
// produces unified diffs against the current state, so the orchestrator can
// keep the model grounded in the file-level truth without resending whole
// scene files. Snapshots are session-scoped and main-thread only.

#ifndef AI_SCENE_DIFF_H
#define AI_SCENE_DIFF_H

#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"

namespace AISceneDiff {

// A diff whose changed-line count (added + removed) exceeds this is not sent
// inline; the model is told the scene changed and to use read_scene_file.
constexpr int MAX_DIFF_CHANGED_LINES = 300;
// Context lines around each hunk in the unified output.
constexpr int DIFF_CONTEXT_LINES = 3;
// Myers diff bail-out: edit distance above this counts as "changed substantially".
constexpr int MYERS_MAX_D = 700;

// ---- Snapshot registry ("what the model last saw") ------------------------
bool has_snapshot(const String &p_scene_path);
void set_snapshot(const String &p_scene_path, const String &p_content);
void remove_snapshot(const String &p_scene_path);
Vector<String> get_snapshot_paths();

// ---- Batch mutation tracking ----------------------------------------------
// Scene paths touched by scene-mutating tool calls since the last take.
// Recorded by AI::execute_single_action, consumed by the orchestrator after
// each tool batch.
void note_batch_scene(const String &p_scene_path);
Vector<String> take_batch_scenes();

// ---- Serialization ---------------------------------------------------------
// Serializes the live edited scene (open tab) with the given path to .tscn
// text, matching what the editor's own save would write: ext_resource ids are
// pre-seeded from the on-disk file and the header uid is patched in. Returns
// empty if the scene is not open in any tab or packing fails. LF line endings.
String serialize_open_scene(const String &p_scene_path);

// Raw disk content, CRLF-normalized to LF. Empty if the file doesn't exist.
String read_disk_scene(const String &p_scene_path);

// ---- Diffing ----------------------------------------------------------------
// Unified diff p_old -> p_new.
// Returns: {changed: bool, too_large: bool, diff: String, added: int,
//           removed: int, old_lines: int, new_lines: int}
// When too_large is true, diff is empty and added/removed may be 0 (unknown).
Dictionary compute_diff(const String &p_old, const String &p_new);

// Current live/disk state of the scene vs its snapshot. On change, the
// snapshot is updated to the current state. Adds to compute_diff's result:
// {path: String, missing: bool}. missing=true means the scene is neither open
// nor on disk (snapshot is dropped).
Dictionary diff_scene_against_snapshot(const String &p_scene_path);

// ---- Hidden-context builders (shared by both agent loops) ------------------
// One diff entry rendered for the model (path + counts + unified diff, or the
// missing/too-large fallbacks). Empty when there is nothing to show.
String format_entry(const Dictionary &p_diff);

// User edits since the model last saw each tracked scene. Discards stale batch
// tracking first (a cancelled run must not leak into this one), then diffs
// every snapshot. Returns the complete "[SCENE CHANGES] ..." block, or empty.
// r_changed_scenes receives the per-scene diff dictionaries for UI signals.
String collect_user_changes(Array *r_changed_scenes);

// Consequences of the AI's own scene-mutating tool calls since the last take.
// First sight of a scene stores a baseline instead of diffing. Returns the
// complete "[SCENE UPDATE] ..." block, or empty.
String collect_ai_updates(Array *r_changed_scenes);

} // namespace AISceneDiff

#endif // AI_SCENE_DIFF_H
