/**************************************************************************/
/*  ai_chat_store.h                                                       */
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

#ifndef AI_CHAT_STORE_H
#define AI_CHAT_STORE_H

#include "core/object/ref_counted.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"

// ---------------------------------------------------------------------------
// HistoryItem — canonical unit of the conversation history.
//
// Each item is either a message (has "role") or a context injection (has "type").
// Stored as a Dictionary for direct JSON round-trip. ts is the append timestamp.
// ---------------------------------------------------------------------------
struct HistoryItem {
	int64_t ts = 0;
	Dictionary data;

	bool is_message() const { return data.has("role"); }
	bool is_injection() const { return !data.has("role") && data.has("type"); }
	String role() const { return data.get("role", ""); }
	String injection_type() const { return data.get("type", ""); }

	HistoryItem() {}
	HistoryItem(int64_t p_ts, const Dictionary &p_data) :
			ts(p_ts), data(p_data) {}
};

// ---------------------------------------------------------------------------
// ChatCheckpoint — anchors a rewind point to a user message timestamp.
// Stored in <chat_id>.meta.json, separate from the JSONL item stream.
// ---------------------------------------------------------------------------
struct ChatCheckpoint {
	String checkpoint_id;
	int64_t anchor_ts = 0;     // ts of the user message this checkpoint is anchored to
	int64_t created_at = 0;
	int item_count = 0;        // number of items at checkpoint time
	int undo_action_index = -1;
	bool undo_revert_available = false;

	ChatCheckpoint() {}
	ChatCheckpoint(const String &p_id, int64_t p_anchor_ts, int64_t p_created,
			int p_item_count, int p_undo_idx, bool p_undo_avail) :
			checkpoint_id(p_id), anchor_ts(p_anchor_ts), created_at(p_created),
			item_count(p_item_count), undo_action_index(p_undo_idx),
			undo_revert_available(p_undo_avail) {}
};

// ---------------------------------------------------------------------------
// AIChatStore — JSONL-backed canonical history store.
//
// File layout:
//   user://ai_chat/<chat_id>.jsonl  — item stream (append during run, rewrite on rewind)
//   user://ai_chat/<chat_id>.meta.json — checkpoints + metadata
//
// The no-orphan invariant is enforced by callers (orchestrator writes assistant
// items before tool results, and fills in synthetic cancelled results on cancel).
// ---------------------------------------------------------------------------
class AIChatStore : public RefCounted {
	GDCLASS(AIChatStore, RefCounted);

private:
	Vector<HistoryItem> _items;
	Vector<ChatCheckpoint> _checkpoints;
	String _jsonl_path;  // user://ai_chat/<chat_id>.jsonl
	String _meta_path;   // user://ai_chat/<chat_id>.meta.json
	String _chat_id;     // "chat_1743751234567"

	bool _ensure_directory_exists() const;
	String _generate_checkpoint_id() const;
	bool _save_meta() const;
	bool _load_meta();

protected:
	static void _bind_methods();

public:
	// ---- Multi-chat helpers -----------------------------------------------
	static String get_chat_dir() { return "user://ai_chat"; }
	static String make_chat_path(const String &p_id) { return "user://ai_chat/" + p_id + ".jsonl"; }
	static String generate_chat_id();
	static Vector<String> list_chat_ids(); // Newest first, .jsonl files only

	// ---- Lifecycle -----------------------------------------------------------
	// Set active chat. Clears in-memory state. Call load_items() to populate.
	void set_chat_id(const String &p_id);
	// Convenience: set_chat_id from a .jsonl path (extracts chat_id from basename)
	void set_file_path(const String &p_jsonl_path);

	String get_file_path() const { return _jsonl_path; }
	String get_chat_id() const { return _chat_id; }

	// ---- Item stream ---------------------------------------------------------
	// Load items from JSONL. Returns in-memory list (also available via get_items).
	Vector<HistoryItem> load_items();

	// Append one item to the JSONL file and in-memory list. Returns the stored item.
	HistoryItem append_item(const Dictionary &p_data);

	// Rewrite the JSONL file with a new item list (used on rewind/truncation).
	bool rewrite_items(const Vector<HistoryItem> &p_new_items);

	// Clear all items and delete the JSONL file.
	void clear_items();

	// Accessors
	const Vector<HistoryItem> &get_items() const { return _items; }
	int item_count() const { return _items.size(); }

	// ---- Canonical item builders --------------------------------------------
	// These produce properly-shaped canonical Dictionaries ready for append_item.

	// user message (images are base64 PNG strings, may be empty)
	static Dictionary make_user_item(const String &p_content, const Vector<String> &p_images = Vector<String>());

	// assistant message — content_blocks is [{type:"text",text:"..."} | {type:"tool_call",id,name,args}]
	static Dictionary make_assistant_item(const Array &p_content_blocks);

	// tool result paired to an assistant tool_call by tool_call_id
	// p_content = {status, tool_name, args, result|error|reason}
	static Dictionary make_tool_item(const String &p_tool_call_id, const Dictionary &p_content);

	// context injection: engine state (written for future use, not yet consumed by _build_model_messages)
	static Dictionary make_engine_state_item(bool p_running, int p_error_count,
			const String &p_errors_text, const String &p_screenshot_path = "");

	// context injection: todo state
	static Dictionary make_todo_state_item(const Array &p_tasks);

	// ---- Checkpoints ---------------------------------------------------------
	// anchor_ts: the ts of the user HistoryItem this checkpoint is anchored to
	ChatCheckpoint create_checkpoint(int64_t p_anchor_ts, int p_undo_action_index, bool p_undo_available);
	const ChatCheckpoint *get_checkpoint_for_ts(int64_t p_anchor_ts) const;
	bool truncate_to_checkpoint(const String &p_checkpoint_id);
	const Vector<ChatCheckpoint> &get_checkpoints() const { return _checkpoints; }

	// Find item index by ts (for edit truncation)
	int find_item_index_by_ts(int64_t p_ts) const;
	bool truncate_to_index(int p_index);

	AIChatStore();
	~AIChatStore();
};

#endif // AI_CHAT_STORE_H
