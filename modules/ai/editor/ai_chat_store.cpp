/**************************************************************************/
/*  ai_chat_store.cpp                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "ai_chat_store.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/core_bind.h"
#include "core/os/time.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void AIChatStore::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_file_path"), &AIChatStore::get_file_path);
	ClassDB::bind_method(D_METHOD("get_chat_id"), &AIChatStore::get_chat_id);
	ClassDB::bind_method(D_METHOD("clear_items"), &AIChatStore::clear_items);
	ClassDB::bind_method(D_METHOD("set_file_path", "path"), &AIChatStore::set_file_path);
}

static int64_t _now_ms() {
	int64_t ms = (int64_t)(Time::get_singleton()->get_unix_time_from_system() * 1000.0);
	ms += Time::get_singleton()->get_ticks_usec() % 1000; // sub-ms uniqueness
	return ms;
}

String AIChatStore::generate_chat_id() {
	return vformat("chat_%d", _now_ms());
}

bool AIChatStore::_ensure_directory_exists() const {
	if (!DirAccess::exists("user://ai_chat")) {
		Ref<DirAccess> da = DirAccess::open("user://");
		ERR_FAIL_COND_V(da.is_null(), false);
		Error err = da->make_dir_recursive("ai_chat");
		ERR_FAIL_COND_V_MSG(err != OK, false, "AIChatStore: Failed to create user://ai_chat/");
	}
	return true;
}

String AIChatStore::_generate_checkpoint_id() const {
	return vformat("ckpt_%d", _now_ms());
}

// ---------------------------------------------------------------------------
// Multi-chat helpers
// ---------------------------------------------------------------------------

Vector<String> AIChatStore::list_chat_ids() {
	Vector<String> ids;
	Ref<DirAccess> da = DirAccess::open("user://ai_chat");
	if (da.is_null()) {
		return ids;
	}
	da->list_dir_begin();
	String fname = da->get_next();
	while (!fname.is_empty()) {
		if (!da->current_is_dir() && fname.begins_with("chat_") && fname.ends_with(".jsonl") && !fname.contains(".raw.")) {
			ids.push_back(fname.get_basename()); // "chat_1711200000000"
		}
		fname = da->get_next();
	}
	da->list_dir_end();
	ids.sort();
	ids.reverse(); // newest first
	return ids;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void AIChatStore::set_chat_id(const String &p_id) {
	_chat_id = p_id;
	_jsonl_path = "user://ai_chat/" + p_id + ".jsonl";
	_meta_path = "user://ai_chat/" + p_id + ".meta.json";
	_items.clear();
	_checkpoints.clear();
}

void AIChatStore::set_file_path(const String &p_jsonl_path) {
	// Accept a full .jsonl path and extract the chat_id from the basename
	String basename = p_jsonl_path.get_file().get_basename(); // "chat_123.jsonl" → "chat_123"
	// Also handle the old .json format path gracefully (strip .json, treat as chat_id)
	if (!basename.begins_with("chat_")) {
		// Unexpected path — use as-is
		_chat_id = basename;
		_jsonl_path = p_jsonl_path.ends_with(".jsonl") ? p_jsonl_path : p_jsonl_path.get_base_dir() + "/" + basename + ".jsonl";
	} else {
		_chat_id = basename;
		_jsonl_path = "user://ai_chat/" + _chat_id + ".jsonl";
	}
	_meta_path = "user://ai_chat/" + _chat_id + ".meta.json";
	_items.clear();
	_checkpoints.clear();
}

AIChatStore::AIChatStore() {}
AIChatStore::~AIChatStore() {}

// ---------------------------------------------------------------------------
// JSONL item stream
// ---------------------------------------------------------------------------

Vector<HistoryItem> AIChatStore::load_items() {
	_items.clear();

	if (_jsonl_path.is_empty() || !FileAccess::exists(_jsonl_path)) {
		print_verbose("AIChatStore: No JSONL file found, starting with empty history.");
		_load_meta();
		return _items;
	}

	Ref<FileAccess> file = FileAccess::open(_jsonl_path, FileAccess::READ);
	if (file.is_null()) {
		WARN_PRINT(vformat("AIChatStore: Failed to open '%s'", _jsonl_path));
		_load_meta();
		return _items;
	}

	int line_num = 0;
	while (!file->eof_reached()) {
		String line = file->get_line().strip_edges();
		line_num++;
		if (line.is_empty()) {
			continue;
		}
		JSON json;
		Error err = json.parse(line);
		if (err != OK || json.get_data().get_type() != Variant::DICTIONARY) {
			WARN_PRINT(vformat("AIChatStore: Skipping malformed line %d in '%s'", line_num, _jsonl_path));
			continue;
		}
		Dictionary row = json.get_data();
		int64_t ts = row.get("ts", (int64_t)0);
		if (!row.has("item") || row["item"].get_type() != Variant::DICTIONARY) {
			WARN_PRINT(vformat("AIChatStore: Line %d missing 'item' dict, skipping.", line_num));
			continue;
		}
		Dictionary item_data = row["item"];
		_items.push_back(HistoryItem(ts, item_data));
	}
	file.unref();

	print_verbose(vformat("AIChatStore: Loaded %d items from '%s'", _items.size(), _jsonl_path));
	_load_meta();
	return _items;
}

HistoryItem AIChatStore::append_item(const Dictionary &p_data) {
	if (!_ensure_directory_exists()) {
		WARN_PRINT("AIChatStore: Cannot append — directory creation failed.");
		return HistoryItem();
	}
	ERR_FAIL_COND_V_MSG(_jsonl_path.is_empty(), HistoryItem(), "AIChatStore: No chat path set.");

	int64_t ts = _now_ms();
	HistoryItem item(ts, p_data);
	_items.push_back(item);

	// Build JSONL line
	Dictionary row;
	row["ts"] = ts;
	row["item"] = p_data;
	String line = JSON::stringify(row) + "\n";

	Ref<FileAccess> file = FileAccess::open(_jsonl_path, FileAccess::READ_WRITE);
	if (file.is_null()) {
		// File may not exist yet — create it
		file = FileAccess::open(_jsonl_path, FileAccess::WRITE);
	}
	ERR_FAIL_COND_V_MSG(file.is_null(), item, vformat("AIChatStore: Failed to open '%s' for append.", _jsonl_path));

	file->seek_end();
	file->store_string(line);
	file.unref();

	return item;
}

bool AIChatStore::rewrite_items(const Vector<HistoryItem> &p_new_items) {
	if (!_ensure_directory_exists()) {
		return false;
	}
	ERR_FAIL_COND_V_MSG(_jsonl_path.is_empty(), false, "AIChatStore: No chat path set.");

	Ref<FileAccess> file = FileAccess::open(_jsonl_path, FileAccess::WRITE);
	ERR_FAIL_COND_V_MSG(file.is_null(), false, vformat("AIChatStore: Failed to open '%s' for rewrite.", _jsonl_path));

	for (int i = 0; i < p_new_items.size(); i++) {
		Dictionary row;
		row["ts"] = p_new_items[i].ts;
		row["item"] = p_new_items[i].data;
		file->store_string(JSON::stringify(row) + "\n");
	}
	file.unref();

	_items = p_new_items;
	print_verbose(vformat("AIChatStore: Rewrote '%s' with %d items.", _jsonl_path, _items.size()));
	return true;
}

void AIChatStore::clear_items() {
	_items.clear();
	_checkpoints.clear();

	if (!_jsonl_path.is_empty() && FileAccess::exists(_jsonl_path)) {
		Ref<DirAccess> da = DirAccess::open(_jsonl_path.get_base_dir());
		if (da.is_valid()) {
			da->remove(_jsonl_path.get_file());
		}
	}
	if (!_meta_path.is_empty() && FileAccess::exists(_meta_path)) {
		Ref<DirAccess> da = DirAccess::open(_meta_path.get_base_dir());
		if (da.is_valid()) {
			da->remove(_meta_path.get_file());
		}
	}
}

// ---------------------------------------------------------------------------
// Canonical item builders
// ---------------------------------------------------------------------------

Dictionary AIChatStore::make_user_item(const String &p_content, const Vector<String> &p_images) {
	Dictionary d;
	d["role"] = "user";
	d["content"] = p_content;
	if (!p_images.is_empty()) {
		Array imgs;
		for (int i = 0; i < p_images.size(); i++) {
			imgs.push_back(p_images[i]);
		}
		d["images"] = imgs;
	}
	return d;
}

Dictionary AIChatStore::make_assistant_item(const Array &p_content_blocks) {
	Dictionary d;
	d["role"] = "assistant";
	d["content"] = p_content_blocks;
	return d;
}

Dictionary AIChatStore::make_tool_item(const String &p_tool_call_id, const Dictionary &p_content) {
	Dictionary d;
	d["role"] = "tool";
	d["tool_call_id"] = p_tool_call_id;
	d["content"] = p_content;
	return d;
}

Dictionary AIChatStore::make_engine_state_item(bool p_running, int p_error_count, int p_warning_count,
		const Array &p_errors) {
	Dictionary d;
	d["type"] = "engine_state";
	d["game_running"] = p_running;
	d["error_count"] = p_error_count;
	d["warning_count"] = p_warning_count;
	if (!p_errors.is_empty()) {
		d["errors"] = p_errors;
	}
	d["timestamp"] = _now_ms();
	return d;
}

Dictionary AIChatStore::make_parse_error_state_item(const String &p_file_path, const Array &p_errors) {
	Dictionary d;
	d["type"] = "parse_error_state";
	d["file_path"] = p_file_path;
	d["error_count"] = p_errors.size();
	if (!p_errors.is_empty()) {
		d["errors"] = p_errors;
	}
	d["timestamp"] = _now_ms();
	return d;
}

Dictionary AIChatStore::make_todo_state_item(const Array &p_tasks) {
	Dictionary d;
	d["type"] = "todo_state";
	d["tasks"] = p_tasks;
	d["timestamp"] = _now_ms();
	return d;
}

Dictionary AIChatStore::make_model_info_item(const String &p_model_id, const String &p_provider_name) {
	Dictionary d;
	d["type"] = "model_info";
	d["model_id"] = p_model_id;
	d["provider"] = p_provider_name;
	d["timestamp"] = _now_ms();
	return d;
}

// ---------------------------------------------------------------------------
// Meta file (checkpoints)
// ---------------------------------------------------------------------------

bool AIChatStore::_save_meta() const {
	if (_meta_path.is_empty()) {
		return false;
	}
	if (!_ensure_directory_exists()) {
		return false;
	}

	Array cp_array;
	for (int i = 0; i < _checkpoints.size(); i++) {
		const ChatCheckpoint &cp = _checkpoints[i];
		Dictionary d;
		d["checkpoint_id"] = cp.checkpoint_id;
		d["anchor_ts"] = cp.anchor_ts;
		d["created_at"] = cp.created_at;
		d["item_count"] = cp.item_count;
		d["undo_action_index"] = cp.undo_action_index;
		d["undo_revert_available"] = cp.undo_revert_available;
		cp_array.push_back(d);
	}

	Dictionary root;
	root["checkpoints"] = cp_array;

	Ref<FileAccess> file = FileAccess::open(_meta_path, FileAccess::WRITE);
	ERR_FAIL_COND_V_MSG(file.is_null(), false, vformat("AIChatStore: Failed to write meta '%s'", _meta_path));
	file->store_string(JSON::stringify(root, "\t"));
	file.unref();
	return true;
}

bool AIChatStore::_load_meta() {
	_checkpoints.clear();

	if (_meta_path.is_empty() || !FileAccess::exists(_meta_path)) {
		return true; // No meta is fine
	}

	Ref<FileAccess> file = FileAccess::open(_meta_path, FileAccess::READ);
	if (file.is_null()) {
		return false;
	}

	JSON json;
	Error err = json.parse(file->get_as_text());
	file.unref();
	if (err != OK || json.get_data().get_type() != Variant::DICTIONARY) {
		WARN_PRINT(vformat("AIChatStore: Failed to parse meta '%s'", _meta_path));
		return false;
	}

	Dictionary root = json.get_data();
	if (root.has("checkpoints") && root["checkpoints"].get_type() == Variant::ARRAY) {
		Array cp_array = root["checkpoints"];
		for (int i = 0; i < cp_array.size(); i++) {
			if (cp_array[i].get_type() != Variant::DICTIONARY) {
				continue;
			}
			Dictionary d = cp_array[i];
			ChatCheckpoint cp;
			cp.checkpoint_id = d.get("checkpoint_id", "");
			cp.anchor_ts = d.get("anchor_ts", (int64_t)0);
			cp.created_at = d.get("created_at", (int64_t)0);
			cp.item_count = d.get("item_count", 0);
			cp.undo_action_index = d.get("undo_action_index", -1);
			cp.undo_revert_available = d.get("undo_revert_available", false);
			if (!cp.checkpoint_id.is_empty()) {
				_checkpoints.push_back(cp);
			}
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Checkpoints
// ---------------------------------------------------------------------------

ChatCheckpoint AIChatStore::create_checkpoint(int64_t p_anchor_ts, int p_undo_action_index, bool p_undo_available) {
	// Update if checkpoint already exists for this anchor
	for (int i = 0; i < _checkpoints.size(); i++) {
		if (_checkpoints[i].anchor_ts == p_anchor_ts) {
			_checkpoints.write[i].undo_action_index = p_undo_action_index;
			_checkpoints.write[i].undo_revert_available = p_undo_available;
			_checkpoints.write[i].item_count = _items.size();
			_save_meta();
			return _checkpoints[i];
		}
	}

	ChatCheckpoint cp(
			_generate_checkpoint_id(),
			p_anchor_ts,
			_now_ms(),
			_items.size(),
			p_undo_action_index,
			p_undo_available);
	_checkpoints.push_back(cp);
	_save_meta();
	print_line(vformat("AIChatStore: Created checkpoint '%s' (anchor_ts=%d, items=%d)",
			cp.checkpoint_id, p_anchor_ts, cp.item_count));
	return cp;
}

const ChatCheckpoint *AIChatStore::get_checkpoint_for_ts(int64_t p_anchor_ts) const {
	for (int i = 0; i < _checkpoints.size(); i++) {
		if (_checkpoints[i].anchor_ts == p_anchor_ts) {
			return &_checkpoints[i];
		}
	}
	return nullptr;
}

bool AIChatStore::truncate_to_checkpoint(const String &p_checkpoint_id) {
	const ChatCheckpoint *cp = nullptr;
	for (int i = 0; i < _checkpoints.size(); i++) {
		if (_checkpoints[i].checkpoint_id == p_checkpoint_id) {
			cp = &_checkpoints[i];
			break;
		}
	}
	ERR_FAIL_COND_V_MSG(!cp, false, vformat("AIChatStore: Checkpoint '%s' not found.", p_checkpoint_id));

	int keep = cp->item_count;
	if (keep < 0 || keep > _items.size()) {
		keep = _items.size();
	}

	Vector<HistoryItem> truncated;
	for (int i = 0; i < keep; i++) {
		truncated.push_back(_items[i]);
	}

	// Remove checkpoints that were created after this point
	for (int i = _checkpoints.size() - 1; i >= 0; i--) {
		if (_checkpoints[i].item_count > keep) {
			_checkpoints.remove_at(i);
		}
	}

	bool ok = rewrite_items(truncated);
	if (ok) {
		_save_meta();
	}
	return ok;
}

int AIChatStore::find_item_index_by_ts(int64_t p_ts) const {
	for (int i = 0; i < _items.size(); i++) {
		if (_items[i].ts == p_ts) {
			return i;
		}
	}
	return -1;
}

bool AIChatStore::truncate_to_index(int p_index) {
	ERR_FAIL_COND_V(p_index < 0, false);
	if (p_index >= _items.size()) {
		return true; // nothing to do
	}

	Vector<HistoryItem> truncated;
	for (int i = 0; i < p_index; i++) {
		truncated.push_back(_items[i]);
	}

	// Remove checkpoints beyond the new end
	for (int i = _checkpoints.size() - 1; i >= 0; i--) {
		if (_checkpoints[i].item_count > p_index) {
			_checkpoints.remove_at(i);
		}
	}

	bool ok = rewrite_items(truncated);
	if (ok) {
		_save_meta();
	}
	return ok;
}

// ---------------------------------------------------------------------------
// Screenshot persistence
// ---------------------------------------------------------------------------

String AIChatStore::get_images_dir() const {
	ERR_FAIL_COND_V_MSG(_chat_id.is_empty(), String(), "AIChatStore: No chat_id set.");
	return get_chat_dir().path_join(_chat_id + "_images");
}

String AIChatStore::save_screenshot(const String &p_tool_call_id, const String &p_b64_png) {
	ERR_FAIL_COND_V_MSG(p_tool_call_id.is_empty(), String(), "AIChatStore: Empty tool_call_id for screenshot.");
	ERR_FAIL_COND_V_MSG(p_b64_png.is_empty(), String(), "AIChatStore: Empty base64 for screenshot.");

	String dir = get_images_dir();
	Ref<DirAccess> da = DirAccess::open("user://");
	if (da.is_valid() && !da->dir_exists(dir)) {
		da->make_dir_recursive(dir);
	}

	PackedByteArray png_bytes = CoreBind::Marshalls::get_singleton()->base64_to_raw(p_b64_png);
	ERR_FAIL_COND_V_MSG(png_bytes.is_empty(), String(), "AIChatStore: Failed to decode base64 screenshot.");

	String filename = p_tool_call_id + ".png";
	String full_path = dir.path_join(filename);

	Ref<FileAccess> f = FileAccess::open(full_path, FileAccess::WRITE);
	ERR_FAIL_COND_V_MSG(f.is_null(), String(), vformat("AIChatStore: Failed to open '%s' for writing.", full_path));

	f->store_buffer(png_bytes.ptr(), png_bytes.size());
	f.unref();

	print_line(vformat("AIChatStore: Saved screenshot '%s' (%d bytes)", full_path, png_bytes.size()));
	return filename;
}

String AIChatStore::load_screenshot_b64(const String &p_filename) const {
	ERR_FAIL_COND_V_MSG(p_filename.is_empty(), String(), "AIChatStore: Empty filename for screenshot load.");

	String full_path = get_images_dir().path_join(p_filename);
	if (!FileAccess::exists(full_path)) {
		WARN_PRINT(vformat("AIChatStore: Screenshot file not found: '%s'", full_path));
		return String();
	}

	Ref<FileAccess> f = FileAccess::open(full_path, FileAccess::READ);
	ERR_FAIL_COND_V_MSG(f.is_null(), String(), vformat("AIChatStore: Failed to open '%s' for reading.", full_path));

	uint64_t len = f->get_length();
	PackedByteArray png_bytes;
	png_bytes.resize(len);
	f->get_buffer(png_bytes.ptrw(), len);
	f.unref();

	return CoreBind::Marshalls::get_singleton()->raw_to_base64(png_bytes);
}
