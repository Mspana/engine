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
#include "core/os/time.h"

void AIChatStore::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_transcript_path"), &AIChatStore::get_transcript_path);
	ClassDB::bind_method(D_METHOD("clear_transcript"), &AIChatStore::clear_transcript);
}

String AIChatStore::_get_transcript_dir() const {
	return "user://ai_chat";
}

String AIChatStore::get_transcript_path() const {
	return _get_transcript_dir().path_join("transcript.json");
}

bool AIChatStore::_ensure_directory_exists() const {
	String dir_path = _get_transcript_dir();
	if (!DirAccess::exists(dir_path)) {
		// Use DirAccess with ACCESS_USERDATA for user:// paths
		Ref<DirAccess> da = DirAccess::open("user://");
		if (da.is_null()) {
			ERR_PRINT("AIChatStore: Failed to open user:// directory.");
			return false;
		}
		Error err = da->make_dir_recursive("ai_chat");
		if (err != OK) {
			ERR_PRINT(vformat("AIChatStore: Failed to create directory '%s'. Error: %d", dir_path, err));
			return false;
		}
		print_verbose(vformat("AIChatStore: Created directory '%s'", dir_path));
	}
	return true;
}

Vector<ChatMessage> AIChatStore::load_transcript() {
	messages.clear();
	checkpoints.clear();

	String path = get_transcript_path();
	if (!FileAccess::exists(path)) {
		print_verbose("AIChatStore: No transcript file found, starting with empty transcript.");
		return messages;
	}

	Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
	if (file.is_null()) {
		WARN_PRINT(vformat("AIChatStore: Failed to open transcript file '%s'.", path));
		return messages;
	}

	String json_text = file->get_as_text();
	file.unref();

	JSON json;
	Error err = json.parse(json_text);
	if (err != OK) {
		WARN_PRINT(vformat("AIChatStore: Failed to parse transcript JSON: %s at line %d. Starting with empty transcript.", json.get_error_message(), json.get_error_line()));
		return messages;
	}

	Variant data = json.get_data();
	if (data.get_type() != Variant::DICTIONARY) {
		WARN_PRINT("AIChatStore: Transcript JSON is not a dictionary. Starting with empty transcript.");
		return messages;
	}

	Dictionary root = data;

	// Check version for forward compatibility (v1 and v2 supported)
	int version = root.get("version", 0);
	if (version < 1 || version > TRANSCRIPT_VERSION) {
		WARN_PRINT(vformat("AIChatStore: Transcript version mismatch (expected 1-%d, got %d). Attempting to load anyway.", TRANSCRIPT_VERSION, version));
	}

	if (!root.has("messages") || root["messages"].get_type() != Variant::ARRAY) {
		WARN_PRINT("AIChatStore: Transcript has no 'messages' array. Starting with empty transcript.");
		return messages;
	}

	Array msg_array = root["messages"];
	for (int i = 0; i < msg_array.size(); i++) {
		if (msg_array[i].get_type() != Variant::DICTIONARY) {
			WARN_PRINT(vformat("AIChatStore: Skipping non-dictionary message at index %d.", i));
			continue;
		}

		Dictionary msg_dict = msg_array[i];
		ChatMessage msg;
		msg.id = msg_dict.get("id", 0);
		msg.role = msg_dict.get("role", "");
		msg.content = msg_dict.get("content", "");
		msg.created_at = msg_dict.get("created_at", 0);
		if (msg_dict.has("images") && msg_dict["images"].get_type() == Variant::ARRAY) {
			Array img_array = msg_dict["images"];
			for (int j = 0; j < img_array.size(); j++) {
				if (img_array[j].get_type() == Variant::STRING) {
					msg.images.push_back(img_array[j]);
				}
			}
		}

		if (msg.role.is_empty()) {
			WARN_PRINT(vformat("AIChatStore: Skipping message with empty role at index %d.", i));
			continue;
		}

		messages.push_back(msg);
	}

	// Load checkpoints (v2+)
	if (root.has("checkpoints") && root["checkpoints"].get_type() == Variant::ARRAY) {
		Array cp_array = root["checkpoints"];
		for (int i = 0; i < cp_array.size(); i++) {
			if (cp_array[i].get_type() != Variant::DICTIONARY) {
				continue;
			}

			Dictionary cp_dict = cp_array[i];
			ChatCheckpoint cp;
			cp.checkpoint_id = cp_dict.get("checkpoint_id", "");
			cp.anchor_message_id = cp_dict.get("anchor_message_id", 0);
			cp.created_at = cp_dict.get("created_at", 0);
			cp.transcript_length = cp_dict.get("transcript_length", 0);
			cp.undo_action_index = cp_dict.get("undo_action_index", -1);
			cp.undo_revert_available = cp_dict.get("undo_revert_available", false);

			if (!cp.checkpoint_id.is_empty()) {
				checkpoints.push_back(cp);
			}
		}
		print_verbose(vformat("AIChatStore: Loaded %d checkpoints.", checkpoints.size()));
	}

	print_verbose(vformat("AIChatStore: Loaded %d messages from transcript.", messages.size()));
	return messages;
}

bool AIChatStore::save_transcript() {
	if (!_ensure_directory_exists()) {
		return false;
	}

	// Build JSON structure - messages
	Array msg_array;
	for (int i = 0; i < messages.size(); i++) {
		const ChatMessage &msg = messages[i];
		Dictionary msg_dict;
		msg_dict["id"] = msg.id;
		msg_dict["role"] = msg.role;
		msg_dict["content"] = msg.content;
		msg_dict["created_at"] = msg.created_at;
		if (!msg.images.is_empty()) {
			Array img_array;
			for (int j = 0; j < msg.images.size(); j++) {
				img_array.push_back(msg.images[j]);
			}
			msg_dict["images"] = img_array;
		}
		msg_array.push_back(msg_dict);
	}

	// Build JSON structure - checkpoints
	Array cp_array;
	for (int i = 0; i < checkpoints.size(); i++) {
		const ChatCheckpoint &cp = checkpoints[i];
		Dictionary cp_dict;
		cp_dict["checkpoint_id"] = cp.checkpoint_id;
		cp_dict["anchor_message_id"] = cp.anchor_message_id;
		cp_dict["created_at"] = cp.created_at;
		cp_dict["transcript_length"] = cp.transcript_length;
		cp_dict["undo_action_index"] = cp.undo_action_index;
		cp_dict["undo_revert_available"] = cp.undo_revert_available;
		cp_array.push_back(cp_dict);
	}

	Dictionary root;
	root["version"] = TRANSCRIPT_VERSION;
	root["messages"] = msg_array;
	root["checkpoints"] = cp_array;

	String json_text = JSON::stringify(root, "\t");

	String path = get_transcript_path();
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	if (file.is_null()) {
		ERR_PRINT(vformat("AIChatStore: Failed to open transcript file for writing '%s'.", path));
		return false;
	}

	file->store_string(json_text);
	file.unref();

	print_verbose(vformat("AIChatStore: Saved %d messages and %d checkpoints to transcript.", messages.size(), checkpoints.size()));
	return true;
}

ChatMessage AIChatStore::append_message(const String &p_role, const String &p_content, const Vector<String> &p_images) {
	int64_t now_ms = Time::get_singleton()->get_unix_time_from_system() * 1000;
	// Add microseconds for uniqueness if multiple messages in same millisecond
	now_ms += Time::get_singleton()->get_ticks_usec() % 1000;

	ChatMessage msg(now_ms, p_role, p_content, now_ms);
	msg.images = p_images;
	messages.push_back(msg);

	save_transcript();

	return msg;
}

ChatMessage AIChatStore::append_tool_result(const Dictionary &p_tool_result) {
	// Convert tool result dictionary to JSON string
	String content = JSON::stringify(p_tool_result);
	return append_message("tool", content);
}

void AIChatStore::clear_transcript() {
	messages.clear();
	checkpoints.clear();

	String path = get_transcript_path();
	if (FileAccess::exists(path)) {
		// Use DirAccess for user:// paths
		Ref<DirAccess> da = DirAccess::open(_get_transcript_dir());
		if (da.is_valid()) {
			Error err = da->remove("transcript.json");
			if (err != OK) {
				WARN_PRINT(vformat("AIChatStore: Failed to delete transcript file '%s'. Error: %d", path, err));
			} else {
				print_verbose("AIChatStore: Deleted transcript file.");
			}
		}
	}
}

String AIChatStore::_generate_checkpoint_id() const {
	int64_t now_ms = Time::get_singleton()->get_unix_time_from_system() * 1000;
	now_ms += Time::get_singleton()->get_ticks_usec() % 1000;
	return vformat("chk_%d", now_ms);
}

ChatCheckpoint AIChatStore::create_checkpoint(int64_t p_anchor_message_id, int p_undo_action_index, bool p_undo_available) {
	// Check if checkpoint already exists for this message
	for (int i = 0; i < checkpoints.size(); i++) {
		if (checkpoints[i].anchor_message_id == p_anchor_message_id) {
			// Update existing checkpoint
			checkpoints.write[i].undo_action_index = p_undo_action_index;
			checkpoints.write[i].undo_revert_available = p_undo_available;
			checkpoints.write[i].transcript_length = messages.size();
			save_transcript();
			print_line(vformat("AIChatStore: Updated checkpoint for message %d", p_anchor_message_id));
			return checkpoints[i];
		}
	}

	// Create new checkpoint
	int64_t now_ms = Time::get_singleton()->get_unix_time_from_system() * 1000;
	ChatCheckpoint cp(
		_generate_checkpoint_id(),
		p_anchor_message_id,
		now_ms,
		messages.size(),
		p_undo_action_index,
		p_undo_available
	);

	checkpoints.push_back(cp);
	save_transcript();

	print_line(vformat("AIChatStore: Created checkpoint '%s' for message %d (transcript length: %d, undo index: %d)",
		cp.checkpoint_id, p_anchor_message_id, cp.transcript_length, p_undo_action_index));

	return cp;
}

const ChatCheckpoint *AIChatStore::get_checkpoint_for_message(int64_t p_message_id) const {
	for (int i = 0; i < checkpoints.size(); i++) {
		if (checkpoints[i].anchor_message_id == p_message_id) {
			return &checkpoints[i];
		}
	}
	return nullptr;
}

bool AIChatStore::truncate_to_checkpoint(const String &p_checkpoint_id) {
	// Find the checkpoint
	int checkpoint_idx = -1;
	for (int i = 0; i < checkpoints.size(); i++) {
		if (checkpoints[i].checkpoint_id == p_checkpoint_id) {
			checkpoint_idx = i;
			break;
		}
	}

	if (checkpoint_idx < 0) {
		ERR_PRINT(vformat("AIChatStore: Checkpoint '%s' not found.", p_checkpoint_id));
		return false;
	}

	const ChatCheckpoint &cp = checkpoints[checkpoint_idx];

	// Truncate messages to checkpoint length
	if (cp.transcript_length < messages.size()) {
		int removed_count = messages.size() - cp.transcript_length;
		messages.resize(cp.transcript_length);
		print_line(vformat("AIChatStore: Truncated %d messages (now %d messages)", removed_count, messages.size()));
	}

	// Remove checkpoints that were created after this one
	Vector<ChatCheckpoint> remaining_checkpoints;
	for (int i = 0; i < checkpoints.size(); i++) {
		if (checkpoints[i].created_at <= cp.created_at) {
			remaining_checkpoints.push_back(checkpoints[i]);
		}
	}
	checkpoints = remaining_checkpoints;

	save_transcript();

	print_line(vformat("AIChatStore: Rewound to checkpoint '%s' (anchor message %d)", p_checkpoint_id, cp.anchor_message_id));
	return true;
}

int AIChatStore::find_message_index(int64_t p_message_id) const {
	for (int i = 0; i < messages.size(); i++) {
		if (messages[i].id == p_message_id) {
			return i;
		}
	}
	return -1;
}

bool AIChatStore::truncate_to_index(int p_index) {
	if (p_index < 0 || p_index > messages.size()) {
		ERR_PRINT(vformat("AIChatStore: Invalid truncation index %d (messages: %d)", p_index, messages.size()));
		return false;
	}

	if (p_index < messages.size()) {
		int removed_count = messages.size() - p_index;
		messages.resize(p_index);
		print_line(vformat("AIChatStore: Truncated to index %d, removed %d messages (now %d messages)", p_index, removed_count, messages.size()));
	}

	// Remove checkpoints that reference messages beyond the new length
	Vector<ChatCheckpoint> remaining_checkpoints;
	for (int i = 0; i < checkpoints.size(); i++) {
		// Keep checkpoints whose transcript_length is <= our new length
		// This ensures we don't keep checkpoints that would try to restore more messages than we have
		if (checkpoints[i].transcript_length <= p_index) {
			remaining_checkpoints.push_back(checkpoints[i]);
		}
	}

	int removed_checkpoints = checkpoints.size() - remaining_checkpoints.size();
	if (removed_checkpoints > 0) {
		print_line(vformat("AIChatStore: Removed %d checkpoints that referenced truncated messages", removed_checkpoints));
	}
	checkpoints = remaining_checkpoints;

	save_transcript();
	return true;
}

AIChatStore::AIChatStore() {
}

AIChatStore::~AIChatStore() {
}
