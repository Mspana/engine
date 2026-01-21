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

	// Check version for forward compatibility
	int version = root.get("version", 0);
	if (version != TRANSCRIPT_VERSION) {
		WARN_PRINT(vformat("AIChatStore: Transcript version mismatch (expected %d, got %d). Attempting to load anyway.", TRANSCRIPT_VERSION, version));
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

		if (msg.role.is_empty()) {
			WARN_PRINT(vformat("AIChatStore: Skipping message with empty role at index %d.", i));
			continue;
		}

		messages.push_back(msg);
	}

	print_verbose(vformat("AIChatStore: Loaded %d messages from transcript.", messages.size()));
	return messages;
}

bool AIChatStore::save_transcript() {
	if (!_ensure_directory_exists()) {
		return false;
	}

	// Build JSON structure
	Array msg_array;
	for (int i = 0; i < messages.size(); i++) {
		const ChatMessage &msg = messages[i];
		Dictionary msg_dict;
		msg_dict["id"] = msg.id;
		msg_dict["role"] = msg.role;
		msg_dict["content"] = msg.content;
		msg_dict["created_at"] = msg.created_at;
		msg_array.push_back(msg_dict);
	}

	Dictionary root;
	root["version"] = TRANSCRIPT_VERSION;
	root["messages"] = msg_array;

	String json_text = JSON::stringify(root, "\t");

	String path = get_transcript_path();
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	if (file.is_null()) {
		ERR_PRINT(vformat("AIChatStore: Failed to open transcript file for writing '%s'.", path));
		return false;
	}

	file->store_string(json_text);
	file.unref();

	print_verbose(vformat("AIChatStore: Saved %d messages to transcript.", messages.size()));
	return true;
}

ChatMessage AIChatStore::append_message(const String &p_role, const String &p_content) {
	int64_t now_ms = Time::get_singleton()->get_unix_time_from_system() * 1000;
	// Add microseconds for uniqueness if multiple messages in same millisecond
	now_ms += Time::get_singleton()->get_ticks_usec() % 1000;

	ChatMessage msg(now_ms, p_role, p_content, now_ms);
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

AIChatStore::AIChatStore() {
}

AIChatStore::~AIChatStore() {
}
