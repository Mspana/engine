// modules/ai/scene_diff.cpp
// Scene snapshot + diff support for the AI module. See scene_diff.h.

#include "scene_diff.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/io/resource_uid.h"
#include "core/templates/hash_map.h"
#include "editor/editor_data.h"
#include "editor/editor_node.h"
#include "editor/editor_paths.h"
#include "scene/resources/packed_scene.h"

namespace AISceneDiff {

// ---------------------------------------------------------------------------
// Snapshot registry + batch tracking (main thread only)
// ---------------------------------------------------------------------------

static HashMap<String, String> g_snapshots;
static Vector<String> g_batch_scenes;

bool has_snapshot(const String &p_scene_path) {
	return g_snapshots.has(p_scene_path);
}

void set_snapshot(const String &p_scene_path, const String &p_content) {
	g_snapshots[p_scene_path] = p_content;
}

void remove_snapshot(const String &p_scene_path) {
	g_snapshots.erase(p_scene_path);
}

Vector<String> get_snapshot_paths() {
	Vector<String> paths;
	for (const KeyValue<String, String> &E : g_snapshots) {
		paths.push_back(E.key);
	}
	return paths;
}

void note_batch_scene(const String &p_scene_path) {
	if (p_scene_path.is_empty() || g_batch_scenes.has(p_scene_path)) {
		return;
	}
	g_batch_scenes.push_back(p_scene_path);
}

Vector<String> take_batch_scenes() {
	Vector<String> out = g_batch_scenes;
	g_batch_scenes.clear();
	return out;
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

// Extracts attr="value" from a single tag line. Returns empty if absent.
static String _extract_attribute(const String &p_line, const String &p_attr) {
	String needle = " " + p_attr + "=\"";
	int start = p_line.find(needle);
	if (start == -1) {
		return String();
	}
	start += needle.length();
	int end = p_line.find("\"", start);
	if (end == -1) {
		return String();
	}
	return p_line.substr(start, end - start);
}

// The text saver assigns ext_resource ids from each resource's per-path id
// cache. That cache is keyed by the save path, so saving to a temp path would
// generate fresh random ids and every diff against disk-format text would be
// noise. Pre-seed the temp path's id cache from the on-disk file's mapping so
// the serialized text carries the same ids as the file.
static void _preseed_ext_ids_from_disk(const String &p_scene_path, const String &p_temp_path) {
	String disk = FileAccess::exists(p_scene_path) ? FileAccess::get_file_as_string(p_scene_path) : String();
	if (disk.is_empty()) {
		return;
	}
	String temp_local = ProjectSettings::get_singleton()->localize_path(p_temp_path);
	Vector<String> lines = disk.split("\n");
	for (const String &line : lines) {
		if (!line.begins_with("[ext_resource")) {
			continue;
		}
		String res_path = _extract_attribute(line, "path");
		String res_id = _extract_attribute(line, "id");
		if (res_path.is_empty() || res_id.is_empty()) {
			continue;
		}
		Ref<Resource> res = ResourceCache::get_ref(res_path);
		if (res.is_valid()) {
			res->set_id_for_path(temp_local, res_id);
		}
	}
}

String serialize_open_scene(const String &p_scene_path) {
	EditorNode *editor = EditorNode::get_singleton();
	if (!editor || p_scene_path.is_empty()) {
		return String();
	}

	EditorData &editor_data = EditorNode::get_editor_data();
	Node *root = nullptr;
	for (int i = 0; i < editor_data.get_edited_scene_count(); i++) {
		Node *r = editor_data.get_edited_scene_root(i);
		if (r && r->get_scene_file_path() == p_scene_path) {
			root = r;
			break;
		}
	}
	if (!root) {
		return String();
	}

	// Fresh PackedScene — never the cached one at p_scene_path: pack() into the
	// cached resource would silently retarget what new instantiations produce.
	Ref<PackedScene> packed;
	packed.instantiate();
	if (packed->pack(root) != OK) {
		return String();
	}

	// Temp path outside res://: never scanned, no uid registered, and the
	// saver force-disables subresource path takeover for non-res:// targets.
	String temp_path = EditorPaths::get_singleton()->get_temp_dir().path_join("ai_scene_snapshot.tscn");
	_preseed_ext_ids_from_disk(p_scene_path, temp_path);

	if (ResourceSaver::save(packed, temp_path) != OK) {
		return String();
	}
	String text = FileAccess::get_file_as_string(temp_path);
	DirAccess::remove_absolute(temp_path);
	if (text.is_empty()) {
		return String();
	}
	text = text.replace("\r\n", "\n");

	// Saves outside res:// omit the header uid; patch in the scene's real uid
	// so serialized text matches what the editor writes to disk.
	if (text.begins_with("[gd_scene")) {
		int header_end = text.find("]");
		if (header_end != -1 && !text.substr(0, header_end).contains(" uid=\"")) {
			ResourceUID::ID uid = ResourceLoader::get_resource_uid(p_scene_path);
			if (uid != ResourceUID::INVALID_ID) {
				String uid_attr = " uid=\"" + ResourceUID::get_singleton()->id_to_text(uid) + "\"";
				text = text.substr(0, header_end) + uid_attr + text.substr(header_end);
			}
		}
	}

	return text;
}

String read_disk_scene(const String &p_scene_path) {
	if (!FileAccess::exists(p_scene_path)) {
		return String();
	}
	String text = FileAccess::get_file_as_string(p_scene_path);
	return text.replace("\r\n", "\n");
}

// ---------------------------------------------------------------------------
// Myers line diff
// ---------------------------------------------------------------------------

enum DiffOpType {
	DIFF_KEEP,
	DIFF_DELETE,
	DIFF_INSERT,
};

struct DiffOp {
	DiffOpType type = DIFF_KEEP;
	int a_index = 0; // line index into a (KEEP/DELETE)
	int b_index = 0; // line index into b (KEEP/INSERT)
};

// Myers O(ND) with trace, bounded by p_max_d. Returns false when the edit
// distance exceeds the bound. Ops are emitted in order.
static bool _myers(const Vector<String> &a, const Vector<String> &b, int p_max_d, Vector<DiffOp> &r_ops) {
	const int n = a.size();
	const int m = b.size();
	const int max_d = MIN(n + m, p_max_d);
	const int offset = max_d;

	Vector<int> v;
	v.resize(2 * max_d + 2);
	v.fill(0);

	Vector<Vector<int>> trace;
	int final_d = -1;

	for (int d = 0; d <= max_d && final_d == -1; d++) {
		trace.push_back(v);
		for (int k = -d; k <= d; k += 2) {
			int x;
			if (k == -d || (k != d && v[offset + k - 1] < v[offset + k + 1])) {
				x = v[offset + k + 1];
			} else {
				x = v[offset + k - 1] + 1;
			}
			int y = x - k;
			while (x < n && y < m && a[x] == b[y]) {
				x++;
				y++;
			}
			v.write[offset + k] = x;
			if (x >= n && y >= m) {
				final_d = d;
				break;
			}
		}
	}

	if (final_d == -1) {
		return false;
	}

	// Backtrack. trace[d] holds V as it was before iteration d.
	Vector<DiffOp> reversed;
	int x = n;
	int y = m;
	for (int d = final_d; d > 0; d--) {
		const Vector<int> &pv = trace[d];
		int k = x - y;
		int prev_k;
		if (k == -d || (k != d && pv[offset + k - 1] < pv[offset + k + 1])) {
			prev_k = k + 1;
		} else {
			prev_k = k - 1;
		}
		int prev_x = pv[offset + prev_k];
		int prev_y = prev_x - prev_k;
		while (x > prev_x && y > prev_y) {
			x--;
			y--;
			reversed.push_back({ DIFF_KEEP, x, y });
		}
		if (x == prev_x) {
			y--;
			reversed.push_back({ DIFF_INSERT, x, y });
		} else {
			x--;
			reversed.push_back({ DIFF_DELETE, x, y });
		}
	}
	while (x > 0 && y > 0) {
		x--;
		y--;
		reversed.push_back({ DIFF_KEEP, x, y });
	}

	r_ops.resize(reversed.size());
	for (int i = 0; i < reversed.size(); i++) {
		r_ops.write[i] = reversed[reversed.size() - 1 - i];
	}
	return true;
}

// Formats ops as a unified diff with hunk headers (1-based line numbers).
static String _format_unified(const Vector<String> &a, const Vector<String> &b, const Vector<DiffOp> &ops,
		int p_context, int *r_added, int *r_removed) {
	*r_added = 0;
	*r_removed = 0;

	// Indexes of non-KEEP ops.
	Vector<int> changes;
	for (int i = 0; i < ops.size(); i++) {
		if (ops[i].type != DIFF_KEEP) {
			changes.push_back(i);
			if (ops[i].type == DIFF_INSERT) {
				(*r_added)++;
			} else {
				(*r_removed)++;
			}
		}
	}
	if (changes.is_empty()) {
		return String();
	}

	String out;
	int group_start = 0;
	while (group_start < changes.size()) {
		// Grow the group while gaps between changes fit inside shared context.
		int group_end = group_start;
		while (group_end + 1 < changes.size() &&
				changes[group_end + 1] - changes[group_end] <= 2 * p_context + 1) {
			group_end++;
		}

		int hunk_lo = MAX(0, changes[group_start] - p_context);
		int hunk_hi = MIN(ops.size() - 1, changes[group_end] + p_context);

		int a_start = 0;
		int b_start = 0;
		int a_count = 0;
		int b_count = 0;
		for (int i = hunk_lo; i <= hunk_hi; i++) {
			if (i == hunk_lo) {
				a_start = ops[i].a_index + 1;
				b_start = ops[i].b_index + 1;
			}
			if (ops[i].type != DIFF_INSERT) {
				a_count++;
			}
			if (ops[i].type != DIFF_DELETE) {
				b_count++;
			}
		}

		out += vformat("@@ -%d,%d +%d,%d @@\n", a_start, a_count, b_start, b_count);
		for (int i = hunk_lo; i <= hunk_hi; i++) {
			switch (ops[i].type) {
				case DIFF_KEEP:
					out += " " + a[ops[i].a_index] + "\n";
					break;
				case DIFF_DELETE:
					out += "-" + a[ops[i].a_index] + "\n";
					break;
				case DIFF_INSERT:
					out += "+" + b[ops[i].b_index] + "\n";
					break;
			}
		}

		group_start = group_end + 1;
	}
	return out;
}

static Vector<String> _split_lines(const String &p_text) {
	Vector<String> lines = p_text.split("\n");
	// Files end with a trailing newline; drop the phantom empty last element
	// so it doesn't show up as a context/changed line at EOF.
	if (!lines.is_empty() && lines[lines.size() - 1].is_empty()) {
		lines.remove_at(lines.size() - 1);
	}
	return lines;
}

Dictionary compute_diff(const String &p_old, const String &p_new) {
	Dictionary out;
	out["changed"] = false;
	out["too_large"] = false;
	out["diff"] = String();
	out["added"] = 0;
	out["removed"] = 0;

	Vector<String> a = _split_lines(p_old);
	Vector<String> b = _split_lines(p_new);
	out["old_lines"] = a.size();
	out["new_lines"] = b.size();

	if (p_old == p_new) {
		return out;
	}
	out["changed"] = true;

	// Trim common prefix/suffix before Myers — scene edits are localized, so
	// this shrinks the problem to the changed region.
	int prefix = 0;
	while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix]) {
		prefix++;
	}
	int suffix = 0;
	while (suffix < a.size() - prefix && suffix < b.size() - prefix &&
			a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix]) {
		suffix++;
	}

	Vector<String> a_mid;
	for (int i = prefix; i < a.size() - suffix; i++) {
		a_mid.push_back(a[i]);
	}
	Vector<String> b_mid;
	for (int i = prefix; i < b.size() - suffix; i++) {
		b_mid.push_back(b[i]);
	}

	Vector<DiffOp> mid_ops;
	if (!_myers(a_mid, b_mid, MYERS_MAX_D, mid_ops)) {
		out["too_large"] = true;
		return out;
	}

	// Reassemble the full op list so hunks can include prefix/suffix context.
	Vector<DiffOp> ops;
	for (int i = 0; i < prefix; i++) {
		ops.push_back({ DIFF_KEEP, i, i });
	}
	for (const DiffOp &op : mid_ops) {
		ops.push_back({ op.type, op.a_index + prefix, op.b_index + prefix });
	}
	for (int i = 0; i < suffix; i++) {
		ops.push_back({ DIFF_KEEP, (int)(a.size() - suffix) + i, (int)(b.size() - suffix) + i });
	}

	int added = 0;
	int removed = 0;
	String diff = _format_unified(a, b, ops, DIFF_CONTEXT_LINES, &added, &removed);
	out["added"] = added;
	out["removed"] = removed;

	if (added + removed == 0) {
		// Only sub-line noise (e.g. trailing newline) — nothing worth reporting.
		out["changed"] = false;
		return out;
	}
	if (added + removed > MAX_DIFF_CHANGED_LINES) {
		out["too_large"] = true;
		return out;
	}
	out["diff"] = diff;
	return out;
}

Dictionary diff_scene_against_snapshot(const String &p_scene_path) {
	Dictionary out;
	out["path"] = p_scene_path;
	out["changed"] = false;
	out["missing"] = false;

	if (!g_snapshots.has(p_scene_path)) {
		return out;
	}

	String current = serialize_open_scene(p_scene_path);
	if (current.is_empty()) {
		current = read_disk_scene(p_scene_path);
	}
	if (current.is_empty()) {
		out["missing"] = true;
		g_snapshots.erase(p_scene_path);
		return out;
	}

	Dictionary diff = compute_diff(g_snapshots[p_scene_path], current);
	for (const Variant *key = diff.next(nullptr); key; key = diff.next(key)) {
		out[*key] = diff[*key];
	}
	if ((bool)out.get("changed", false)) {
		g_snapshots[p_scene_path] = current;
	}
	return out;
}

// ---------------------------------------------------------------------------
// Hidden-context builders

String format_entry(const Dictionary &p_diff) {
	String path = p_diff.get("path", "");
	if (path.is_empty()) {
		return String();
	}
	if ((bool)p_diff.get("missing", false)) {
		return vformat("%s: no longer exists (deleted or renamed).", path);
	}
	if ((bool)p_diff.get("too_large", false)) {
		return vformat("%s: changed substantially (now %d lines, was %d) - too large to show inline. Use read_scene_file to see the current state.",
				path, (int)p_diff.get("new_lines", 0), (int)p_diff.get("old_lines", 0));
	}
	String diff_text = p_diff.get("diff", "");
	if (diff_text.is_empty()) {
		return String();
	}
	return vformat("%s (+%d/-%d lines):\n%s", path,
			(int)p_diff.get("added", 0), (int)p_diff.get("removed", 0), diff_text.strip_edges());
}

String collect_user_changes(Array *r_changed_scenes) {
	// Discard stale batch tracking from a previous (e.g. cancelled) run.
	take_batch_scenes();

	Vector<String> tracked = get_snapshot_paths();
	String block;
	for (const String &scene_path : tracked) {
		Dictionary diff = diff_scene_against_snapshot(scene_path);
		if (!(bool)diff.get("missing", false) && !(bool)diff.get("changed", false)) {
			continue;
		}
		String entry = format_entry(diff);
		if (entry.is_empty()) {
			continue;
		}
		block += entry + "\n";
		if (r_changed_scenes) {
			r_changed_scenes->push_back(diff);
		}
	}
	if (block.is_empty()) {
		return String();
	}
	return "[SCENE CHANGES] The following scene file(s) changed outside this conversation (user edits in the editor, or external changes) since you last saw them:\n\n" + block.strip_edges();
}

String collect_ai_updates(Array *r_changed_scenes) {
	Vector<String> paths = take_batch_scenes();
	if (paths.is_empty()) {
		return String();
	}
	String block;
	for (const String &scene_path : paths) {
		if (!has_snapshot(scene_path)) {
			// First sight of this scene (e.g. just created): store a baseline,
			// nothing to diff against yet.
			String text = serialize_open_scene(scene_path);
			if (text.is_empty()) {
				text = read_disk_scene(scene_path);
			}
			if (!text.is_empty()) {
				set_snapshot(scene_path, text);
			}
			continue;
		}
		Dictionary diff = diff_scene_against_snapshot(scene_path);
		if (!(bool)diff.get("changed", false) && !(bool)diff.get("missing", false)) {
			continue;
		}
		String entry = format_entry(diff);
		if (entry.is_empty()) {
			continue;
		}
		block += entry + "\n";
		if (r_changed_scenes) {
			r_changed_scenes->push_back(diff);
		}
	}
	if (block.is_empty()) {
		return String();
	}
	return "[SCENE UPDATE] Resulting scene file changes from your tool calls:\n\n" + block.strip_edges();
}

} // namespace AISceneDiff
