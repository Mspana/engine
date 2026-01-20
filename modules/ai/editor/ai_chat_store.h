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
	String role;              // "user" | "assistant" | "system"
	String content;           // Message text
	int64_t created_at = 0;   // Unix timestamp ms

	ChatMessage() {}
	ChatMessage(int64_t p_id, const String &p_role, const String &p_content, int64_t p_created_at)
		: id(p_id), role(p_role), content(p_content), created_at(p_created_at) {}
};

// Handles persistence of chat transcript to disk
class AIChatStore : public RefCounted {
	GDCLASS(AIChatStore, RefCounted);

private:
	static const int TRANSCRIPT_VERSION = 1;
	Vector<ChatMessage> messages;

	String _get_transcript_dir() const;
	bool _ensure_directory_exists() const;

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

	// Clear all messages and delete the transcript file
	void clear_transcript();

	// Get current in-memory messages
	const Vector<ChatMessage> &get_messages() const { return messages; }

	// Set messages (used when loading)
	void set_messages(const Vector<ChatMessage> &p_messages) { messages = p_messages; }

	AIChatStore();
	~AIChatStore();
};

#endif // AI_CHAT_STORE_H
