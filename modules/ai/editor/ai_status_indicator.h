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
#include "scene/gui/rich_text_label.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/separator.h"
#include "scene/gui/text_edit.h"
#include "scene/gui/texture_rect.h"
#include "scene/gui/check_box.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/popup.h"
#include "scene/main/http_request.h"
#include "scene/main/timer.h"
#include "core/input/input_event.h"
#include "core/io/image.h"
#include "scene/resources/image_texture.h"

// Lightweight collapsible entry for agent thinking text between tool calls
class ThinkingCollapsibleEntry : public VBoxContainer {
	GDCLASS(ThinkingCollapsibleEntry, VBoxContainer);

private:
	bool is_collapsed = true;
	Button *toggle_button = nullptr;
	RichTextLabel *body_label = nullptr;

	void _on_toggle_pressed();

protected:
	static void _bind_methods();

public:
	void set_text(const String &p_text);
	ThinkingCollapsibleEntry();
};

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
	TextureRect *body_screenshot = nullptr;
	String screenshot_b64;

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

	TextureRect *get_screenshot_widget() const { return body_screenshot; }
	String get_screenshot_b64() const { return screenshot_b64; }

	ToolCollapsibleEntry();
};

// Compact panel showing the AI's self-managed task list during a run
class AITodoPanelWidget : public PanelContainer {
	GDCLASS(AITodoPanelWidget, PanelContainer);

	VBoxContainer *items_container = nullptr;
	Label *progress_label = nullptr;

protected:
	static void _bind_methods() {}

public:
	AITodoPanelWidget();
	void update_todos(const Array &p_todos);
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

	// Message queue (in-memory, not persisted)
	Vector<QueuedMessage> message_queue;
	RunState run_state = STATE_IDLE;

	// Todo panel (AI's self-managed task list)
	AITodoPanelWidget *todo_panel = nullptr;

	// Queue display UI (simple list above input)
	VBoxContainer *queue_container = nullptr;
	Label *queue_header_label = nullptr;

	// Status bar
	AIStatusIndicator *status_indicator = nullptr;
	Label *status_label = nullptr;
	Label *context_usage_label = nullptr;

	// HTTP requests for checking each provider
	HTTPRequest *http_openai = nullptr;
	HTTPRequest *http_gemini = nullptr;
	HTTPRequest *http_xai = nullptr;

	// Track check results
	bool openai_connected = false;
	bool gemini_connected = false;
	bool xai_connected = false;
	int pending_checks = 0;

	// Auto-scroll state
	bool should_auto_scroll = true;

	// Chat state
	bool is_waiting_for_response = false;
	bool context_was_truncated = false;
	int64_t current_run_user_message_id = 0; // User message ID for checkpoint anchoring

	// Rewind/Edit UI - custom dialog with three buttons
	AcceptDialog *rewind_dialog = nullptr;
	Label *rewind_dialog_label = nullptr; // Dynamic label for dialog text
	CheckBox *dont_ask_again_checkbox = nullptr;
	Button *cancel_button = nullptr;
	Button *continue_no_revert_button = nullptr;
	Button *continue_revert_button = nullptr;
	int64_t pending_rewind_message_id = 0;
	String pending_rewind_checkpoint_id;

	// "Don't ask again" preference for edit sends
	bool skip_edit_send_dialog = false;

	// Edit mode - track state for showing dialog on send
	bool is_pending_edit_send = false; // True after edit rewind, until send completes
	int undo_target_for_edit = -1; // UndoRedo index to revert to if user chooses
	bool undo_available_for_edit = false;

	// Pending images (staged for next send, cleared after _start_run)
	Vector<String> pending_images;          // base64-encoded PNG strings (512px max)
	Vector<Ref<Image>> pending_images_raw;  // kept for thumbnail display in preview strip

	// Image preview strip (shown above input bar when images are pending)
	HBoxContainer *image_preview_strip = nullptr;

	// Image lightbox popup (click thumbnail to enlarge)
	PopupPanel *image_popup = nullptr;
	TextureRect *image_popup_tex = nullptr;

	// Build messages array for API call with truncation
	Array _build_model_messages();

	// Image paste and preview
	void _remove_pending_image(int p_index);
	void _clear_pending_images();
	void _rebuild_image_preview_strip();
	void _show_image_popup(const String &p_base64);
	void _on_thumbnail_gui_input(const Ref<InputEvent> &p_event, const String &p_base64);

	// Context usage indicator
	void _update_context_usage(int p_used_chars, int p_max_chars);
	void _reset_context_usage();
	void _refresh_context_usage(); // Recompute from chat store (use after run complete / rewind)

	// UI building methods
	void _rebuild_message_list();
	void _append_message_ui(const ChatMessage &p_message);
	Control *_create_message_bubble(const ChatMessage &p_message);
	Control *_create_tool_result_ui(const Dictionary &p_tool_result);
	void _append_tool_result_ui(const Dictionary &p_tool_result);
	void _append_thinking_ui(const String &p_text);
	Control *_create_narration_bubble(const String &p_text);
	void _scroll_to_bottom();
	void _on_scrollbar_range_changed();
	void _on_vscroll_changed(float p_value);
	void _update_send_button_state();
	void _update_queue_ui();
	void _rebuild_queue_list();
	Control *_create_queue_item(int p_index, const QueuedMessage &p_msg);
	void _on_queue_item_edit(int p_index);
	void _on_queue_item_remove(int p_index);

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
	void _on_orchestrator_started();
	void _on_orchestrator_progress(const String &p_status, int p_turn);
	void _on_orchestrator_tool_result(const Dictionary &p_tool_result);
	void _on_orchestrator_complete(bool p_success, const String &p_final_message);
	void _on_orchestrator_narration(const String &p_text);
	void _on_todos_updated(const Array &p_todos);

	// Pending message helpers
	void _show_pending_message();
	void _remove_pending_message();

	// Rewind functionality
	void _on_rewind_clicked(int64_t p_message_id);
	void _on_dialog_cancel();
	void _on_dialog_continue_no_revert();
	void _on_dialog_continue_revert();
	void _perform_rewind(const String &p_checkpoint_id, bool p_revert_project);
	void _revert_project_to_checkpoint(const ChatCheckpoint &p_checkpoint);

	// Edit functionality (rewind + prefill input, dialog on send)
	void _on_edit_clicked(int64_t p_message_id);
	void _cancel_pending_edit();

	// Checkpoint creation (called when orchestrator recommends)
	void _on_checkpoint_recommended(int64_t p_user_message_id);

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
	void _add_pending_image(Ref<Image> p_image); // Public: called via AI signal from game_view_plugin

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
