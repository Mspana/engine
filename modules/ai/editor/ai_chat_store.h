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

// Message data structure for chat transcript
struct ChatMessage {
	int64_t id = 0;           // Unix timestamp ms (unique enough for single user)
	String role;              // "user" | "assistant" | "system" | "tool"
	String content;           // Message text (for "tool" role, this is JSON-stringified tool result)
	int64_t created_at = 0;   // Unix timestamp ms

	ChatMessage() {}
	ChatMessage(int64_t p_id, const String &p_role, const String &p_content, int64_t p_created_at)
		: id(p_id), role(p_role), content(p_content), created_at(p_created_at) {}
};

// Checkpoint data structure for rewind functionality
struct ChatCheckpoint {
	String checkpoint_id;             // Unique ID (timestamp-based)
	int64_t anchor_message_id = 0;    // The user message ID this checkpoint is anchored to
	int64_t created_at = 0;           // When checkpoint was created
	int transcript_length = 0;        // Number of messages at checkpoint time
	int undo_action_index = -1;       // EditorUndoRedoManager action index at checkpoint
	bool undo_revert_available = false; // Whether UndoRedo revert is possible

	ChatCheckpoint() {}
	ChatCheckpoint(const String &p_id, int64_t p_anchor_id, int64_t p_created, int p_length, int p_undo_idx, bool p_undo_avail)
		: checkpoint_id(p_id), anchor_message_id(p_anchor_id), created_at(p_created),
		  transcript_length(p_length), undo_action_index(p_undo_idx), undo_revert_available(p_undo_avail) {}
};

// Handles persistence of chat transcript to disk
class AIChatStore : public RefCounted {
	GDCLASS(AIChatStore, RefCounted);

private:
	static const int TRANSCRIPT_VERSION = 2; // v2 adds checkpoints
	Vector<ChatMessage> messages;
	Vector<ChatCheckpoint> checkpoints;

	String _get_transcript_dir() const;
	bool _ensure_directory_exists() const;
	String _generate_checkpoint_id() const;

protected:
	static void _bind_methods();

public:
	// Returns the path to the transcript file
	String get_transcript_path() const;

	// Load transcript from disk. Returns empty vector if file missing/corrupt.
	Vector<ChatMessage> load_transcript();

	// Save current transcript to disk
	bool save_transcript();

	// Append a new message and save. Returns the created message.
	ChatMessage append_message(const String &p_role, const String &p_content);

	// Append a tool result message (convenience method for agentic tool use)
	ChatMessage append_tool_result(const Dictionary &p_tool_result);

	// Clear all messages and delete the transcript file
	void clear_transcript();

	// Get current in-memory messages
	const Vector<ChatMessage> &get_messages() const { return messages; }

	// Set messages (used when loading)
	void set_messages(const Vector<ChatMessage> &p_messages) { messages = p_messages; }

	// Checkpoint management
	ChatCheckpoint create_checkpoint(int64_t p_anchor_message_id, int p_undo_action_index, bool p_undo_available);
	const ChatCheckpoint *get_checkpoint_for_message(int64_t p_message_id) const;
	bool truncate_to_checkpoint(const String &p_checkpoint_id);
	const Vector<ChatCheckpoint> &get_checkpoints() const { return checkpoints; }

	// Edit support - find message and truncate by index
	int find_message_index(int64_t p_message_id) const;
	bool truncate_to_index(int p_index);

	AIChatStore();
	~AIChatStore();
};

#endif // AI_CHAT_STORE_H
