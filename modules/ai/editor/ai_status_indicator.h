/**************************************************************************/
/*  ai_status_indicator.h                                                 */
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

#ifndef AI_STATUS_INDICATOR_H
#define AI_STATUS_INDICATOR_H

#include "ai_chat_store.h"
#include "editor/plugins/editor_plugin.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/color_rect.h"
#include "scene/gui/label.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/separator.h"
#include "scene/gui/text_edit.h"
#include "scene/main/http_request.h"
#include "scene/main/timer.h"
#include "core/input/input_event.h"

// Collapsible entry for tool results in the chat transcript
class ToolCollapsibleEntry : public VBoxContainer {
	GDCLASS(ToolCollapsibleEntry, VBoxContainer);

private:
	bool is_collapsed = true;

	// Header row (always visible)
	HBoxContainer *header_container = nullptr;
	Label *header_label = nullptr;
	Label *status_label = nullptr;
	Button *toggle_button = nullptr;

	// Body (hidden when collapsed)
	PanelContainer *body_container = nullptr;
	TextEdit *body_text = nullptr;

	void _on_toggle_pressed();
	void _on_header_gui_input(const Ref<InputEvent> &p_event);
	void _update_toggle_icon();

protected:
	static void _bind_methods();

public:
	void set_collapsed(bool p_collapsed);
	bool get_collapsed() const;

	void set_header(const String &p_text, const String &p_status = "");
	void set_body(const String &p_text);

	// Convenience: update from tool result dictionary
	void update_from_tool_result(const Dictionary &p_tool_result);

	ToolCollapsibleEntry();
};

class AIStatusIndicator : public ColorRect {
	GDCLASS(AIStatusIndicator, ColorRect);

public:
	enum Status {
		STATUS_UNKNOWN,
		STATUS_CHECKING,
		STATUS_CONNECTED,
		STATUS_DISCONNECTED
	};

private:
	Status current_status = STATUS_UNKNOWN;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_status(Status p_status);
	Status get_status() const;

	AIStatusIndicator();
};

class AIStatusPanel : public VBoxContainer {
	GDCLASS(AIStatusPanel, VBoxContainer);

public:
	// Run state for the composer
	enum RunState {
		STATE_IDLE,
		STATE_RUNNING,
		STATE_CANCELLING
	};

	// Queued message structure
	struct QueuedMessage {
		String id;
		String text;
		uint64_t created_at;
	};

private:
	// Chat store for persistence
	Ref<AIChatStore> chat_store;

	// Chat transcript UI
	ScrollContainer *transcript_scroll = nullptr;
	VBoxContainer *message_list = nullptr;
	Control *pending_message = nullptr;

	// Input area
	TextEdit *prompt_edit = nullptr;
	Button *send_button = nullptr;  // Toggles between Send/Stop
	Button *clear_button = nullptr;
	Label *queue_count_label = nullptr;  // Shows "Queued: N"

	// Message queue (in-memory, not persisted)
	Vector<QueuedMessage> message_queue;
	RunState run_state = STATE_IDLE;

	// Status bar
	AIStatusIndicator *status_indicator = nullptr;
	Label *status_label = nullptr;

	// HTTP requests for checking each provider
	HTTPRequest *http_openai = nullptr;
	HTTPRequest *http_gemini = nullptr;
	HTTPRequest *http_xai = nullptr;

	// Track check results
	bool openai_connected = false;
	bool gemini_connected = false;
	bool xai_connected = false;
	int pending_checks = 0;

	// Chat state
	bool is_waiting_for_response = false;
	bool context_was_truncated = false;

	// Build messages array for API call with truncation
	Array _build_model_messages();

	// UI building methods
	void _rebuild_message_list();
	void _append_message_ui(const ChatMessage &p_message);
	Control *_create_message_bubble(const ChatMessage &p_message);
	Control *_create_tool_result_ui(const Dictionary &p_tool_result);
	void _append_tool_result_ui(const Dictionary &p_tool_result);
	void _scroll_to_bottom();
	void _update_send_button_state();
	void _update_queue_ui();

	// Message queue management
	void _enqueue_message(const String &p_text);
	void _dequeue_and_run_next();
	void _remove_queued_message(int p_index);
	String _generate_queue_id();

	// Run state management
	void _set_run_state(RunState p_state);
	void _start_run(const String &p_message);
	void _request_cancel();

	// Event handlers
	void _on_send_button_pressed();  // Handles both Send and Stop
	void _on_clear_pressed();
	void _on_prompt_text_changed();
	void _on_prompt_gui_input(const Ref<InputEvent> &p_event);
	void _on_ai_response(bool p_success, const String &p_response, const String &p_error);

	// Agentic orchestrator callbacks
	void _on_orchestrator_progress(const String &p_status, int p_turn);
	void _on_orchestrator_tool_result(const Dictionary &p_tool_result);
	void _on_orchestrator_complete(bool p_success, const String &p_final_message);

	// Pending message helpers
	void _show_pending_message();
	void _remove_pending_message();

	// Connectivity check handlers
	void _on_openai_request_completed(int p_result, int p_code, const PackedStringArray &p_headers, const PackedByteArray &p_body);
	void _on_gemini_request_completed(int p_result, int p_code, const PackedStringArray &p_headers, const PackedByteArray &p_body);
	void _on_xai_request_completed(int p_result, int p_code, const PackedStringArray &p_headers, const PackedByteArray &p_body);
	void _update_status_from_results();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void check_api_connectivity();

	AIStatusPanel();
	~AIStatusPanel();
};

class AIStatusIndicatorPlugin : public EditorPlugin {
	GDCLASS(AIStatusIndicatorPlugin, EditorPlugin);

private:
	AIStatusPanel *panel = nullptr;
	Timer *fallback_timer = nullptr;

	void _on_fallback_timer_timeout();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	virtual String get_plugin_name() const override { return "AI"; }
	virtual bool has_main_screen() const override { return false; }

	AIStatusIndicatorPlugin();
	~AIStatusIndicatorPlugin();
};

VARIANT_ENUM_CAST(AIStatusIndicator::Status);

#endif // AI_STATUS_INDICATOR_H
