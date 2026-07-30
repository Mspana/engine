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
#include "ai_image_widgets.h"
#include "core/templates/hash_map.h"
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
#include "scene/gui/option_button.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/popup.h"
#include "scene/gui/popup_menu.h"
#include "scene/main/http_request.h"
#include "scene/main/timer.h"
#include "core/input/input_event.h"
#include "core/io/image.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/style_box_flat.h"

class CodexHarnessDriver;

// Collapsible entry for tool results in the chat transcript
class ToolCollapsibleEntry : public VBoxContainer {
	GDCLASS(ToolCollapsibleEntry, VBoxContainer);

private:
	bool is_collapsed = true;
	bool is_pending = false; // true between pre-creation and tool_result arrival

	// Header row (always visible)
	HBoxContainer *header_container = nullptr;
	Label *header_label = nullptr;
	Label *token_label = nullptr;
	Label *status_label = nullptr;
	Button *toggle_button = nullptr;
	Ref<StyleBoxFlat> panel_style; // kept to update border color on result

	// Body (hidden when collapsed)
	PanelContainer *body_container = nullptr;
	TextEdit *body_text = nullptr;
	AIImageStack *body_stack = nullptr;
	// Base64 PNGs feeding the stack and (on click) the viewer. Single-image
	// tool results yield a 1-element vector; multi-shot run_and_screenshot and
	// preview_asset populate it with N entries in the order they should be
	// navigated. Empty when no images.
	PackedStringArray screenshot_b64s;

	void _on_toggle_pressed();
	void _on_header_gui_input(const Ref<InputEvent> &p_event);
	void _update_toggle_icon();

	// Height + fade collapse/expand animation, per the AI design language doc.
	// `_collapse_tween` is the currently-running tween (if any); it's killed
	// before a new one starts so rapid toggles don't pile up half-finished
	// transitions. The "finished" callback only runs at the end of a collapse
	// (resets minimum size + visibility back to clean defaults).
	Ref<Tween> _collapse_tween;
	void _animate_to_collapsed(bool p_collapsed);
	void _on_collapse_finished();

protected:
	static void _bind_methods();

public:
	void set_collapsed(bool p_collapsed);
	bool get_collapsed() const;

	void set_header(const String &p_text, const String &p_status = "");
	void set_body(const String &p_text);

	// Convenience: update from tool result dictionary
	void update_from_tool_result(const Dictionary &p_tool_result);

	// Pending state — card is shown immediately when the assistant item arrives,
	// animated by the owning AIStatusPanel until the tool result replaces it.
	bool get_is_pending() const { return is_pending; }
	void set_pending_glyph(const String &p_glyph);

	void set_token_label_visible(bool p_visible);

	AIImageStack *get_image_stack() const { return body_stack; }
	const PackedStringArray &get_screenshot_b64s() const { return screenshot_b64s; }

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

// ============================================================================
// ParseErrorPill — shows parse errors from script actions above the input box
// ============================================================================

class ParseErrorPill : public VBoxContainer {
	GDCLASS(ParseErrorPill, VBoxContainer);

	PanelContainer *_pill_container = nullptr;
	HBoxContainer *_header_row = nullptr;
	RichTextLabel *_main_label = nullptr;
	VBoxContainer *_error_list = nullptr;
	bool _expanded = false;

	Ref<StyleBoxFlat> _style;

	String _file_path;
	Array _errors; // [{line, column, message, type}]

	void _on_header_clicked(const Ref<InputEvent> &p_event);
	void _set_expanded(bool p_expanded);
	void _rebuild_ui();

protected:
	static void _bind_methods();

public:
	void set_errors(const String &p_file_path, const Array &p_errors);
	void clear_for_file(const String &p_file_path);
	void clear_all();
	bool has_content() const { return !_errors.is_empty(); }
	String get_file_path() const { return _file_path; }
	const Array &get_errors() const { return _errors; }
	String get_context_summary() const;
	ParseErrorPill();
};

// ============================================================================
// DebugContextPill — shows game session error state above the input box
// ============================================================================

class DebugContextPill : public VBoxContainer {
	GDCLASS(DebugContextPill, VBoxContainer);

	PanelContainer *_pill_container = nullptr;
	HBoxContainer *_header_row = nullptr;
	RichTextLabel *_main_label = nullptr;
	Button *_toggle_btn = nullptr;

	// Expandable error list
	VBoxContainer *_error_list = nullptr;
	bool _expanded = false;

	Ref<StyleBoxFlat> _style_enabled;
	Ref<StyleBoxFlat> _style_disabled;

	bool _enabled = true;
	int _error_count = 0;
	int _warning_count = 0;
	bool _game_running = false;
	Array _errors; // Structured error dicts from get_structured_errors()

	void _on_toggle_pressed();
	void _on_header_clicked(const Ref<InputEvent> &p_event);
	void _set_expanded(bool p_expanded);
	void _rebuild_label();
	void _rebuild_error_list();

protected:
	static void _bind_methods();

public:
	void update_state(bool p_game_running, int p_error_count, int p_warning_count, const Array &p_errors = Array());
	bool is_enabled() const { return _enabled; }
	bool has_content() const { return (_error_count + _warning_count) > 0; }
	const Array &get_errors() const { return _errors; }
	String get_context_summary() const;
	DebugContextPill();
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
	// Run state for the composer. Cancelling is instant (the orchestrator
	// tears the run down synchronously in cancel_run), so there is no
	// intermediate "stopping" state.
	enum RunState {
		STATE_IDLE,
		STATE_RUNNING
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
	Label *pending_label = nullptr;
	Timer *thinking_dot_timer = nullptr;
	int thinking_dot_state = 0;

	// Pairing map for tool cards: set when an assistant item with tool_call blocks
	// arrives, cleared as each tool_result fires. Member-scoped so the live-run
	// pairing survives across signals (reload uses a local at rebuild time).
	HashMap<String, ToolCollapsibleEntry *> pending_tool_entries;
	Timer *pending_tool_timer = nullptr;
	int pending_tool_spinner_frame = 0;
	void _on_pending_tool_spinner_tick();

	// Input area
	TextEdit *prompt_edit = nullptr;
	Button *send_button = nullptr;  // Toggles between Send/Stop
	Control *input_drag_handle = nullptr;
	bool _dragging_input = false;
	float _drag_start_y = 0;
	float _drag_start_height = 0;
	void _on_drag_handle_gui_input(const Ref<InputEvent> &p_event);

	// Drag-and-drop from the FileSystem dock onto `prompt_edit`. Accepts the
	// FileSystemDock's "files" / "files_and_dirs" payloads and inserts each
	// dropped path at the caret. Wired via SET_DRAG_FORWARDING_CDU.
	bool _can_drop_data_fw(const Point2 &p_point, const Variant &p_data, Control *p_from) const;
	void _drop_data_fw(const Point2 &p_point, const Variant &p_data, Control *p_from);

	// Chat toolbar (top of panel)
	HBoxContainer *chat_toolbar = nullptr;
	Button *new_chat_button = nullptr;
	Button *history_button = nullptr;
	Button *token_toggle_button = nullptr;
	bool _show_token_counts = false;
	int _run_token_total = 0;       // estimated total (fallback)
	int _last_turn_tokens = 0;      // actual total_tokens from API usage field

	// History popup
	PopupPanel *history_popup = nullptr;
	VBoxContainer *history_list = nullptr;

	// Delete chat dialog
	ConfirmationDialog *delete_chat_dialog = nullptr;
	String pending_delete_chat_id;

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
	OptionButton *provider_dropdown = nullptr;

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
	bool context_exhausted = false; // Set when context truncation detected; blocks further sends
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

	// Parse error pill (shown above input bar when script actions have parse errors)
	ParseErrorPill *parse_error_pill = nullptr;
	Control *_create_parse_error_bubble();

	// Debug context pill (shown above input bar when game has errors or is running)
	DebugContextPill *debug_pill = nullptr;
	Timer *debug_pill_update_timer = nullptr;
	void _update_debug_pill();
	void _on_debug_pill_update_tick();
	void _on_debug_context_toggled(bool p_enabled);
	Control *_create_debug_context_bubble();

	// Image preview strip (shown above input bar when images are pending)
	HBoxContainer *image_preview_strip = nullptr;

	// Image lightbox popup (click thumbnail to enlarge). The viewer accepts a
	// vector of base64 PNGs and exposes prev/next navigation when more than
	// one is supplied; a single image looks identical to the old popup.
	AIImageViewer *image_viewer = nullptr;

	// Build messages array for API call with truncation
	Array _build_model_messages();

	// Image paste and preview
	void _remove_pending_image(int p_index);
	void _clear_pending_images();
	void _rebuild_image_preview_strip();
	void _show_image_popup(const PackedStringArray &p_b64s);
	void _on_thumbnail_gui_input(const Ref<InputEvent> &p_event, const String &p_base64);

	// Context usage indicator
	void _update_context_usage(int p_used_chars, int p_max_chars);
	void _reset_context_usage();
	void _refresh_context_usage(); // Recompute from chat store (use after run complete / rewind)

	// Right-click context menu for message bubbles (shared across all bubbles)
	PopupMenu *bubble_context_menu = nullptr;
	String _bubble_menu_text;
	void _on_message_bubble_gui_input(const Ref<InputEvent> &p_event, Control *p_bubble);
	void _on_bubble_menu_id_pressed(int p_id);

	// UI building methods
	void _rebuild_message_list();
	void _append_message_ui(const HistoryItem &p_item);
	Control *_create_message_bubble(const HistoryItem &p_item);
	Control *_create_tool_result_ui(const Dictionary &p_tool_result);
	void _append_tool_result_ui(const Dictionary &p_tool_result);
	void _append_thinking_ui(const String &p_text);
	Control *_create_narration_bubble(const String &p_text);
	Control *_create_cancel_notice();
	Control *_create_error_notice(const String &p_text);
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
	// Returns the generated queue id (used to track the message through the
	// orchestrator's pending-injection list until consumed).
	String _enqueue_message(const String &p_text);
	void _dequeue_and_run_next();
	void _withdraw_pending_injection(const String &p_queue_id);
	void _remove_queued_message(int p_index);
	String _generate_queue_id();

	// Run state management
	void _set_run_state(RunState p_state);
	void _start_run(const String &p_message);
	void _request_cancel();

	// Token count toggle
	void _on_token_toggle_pressed();
	void _insert_token_total_label();
	static void _insert_token_total_label_into(VBoxContainer *p_list, int p_total, Control *p_before = nullptr);

	// Multi-chat management
	void _new_chat();
	void _show_history_popup();
	void _rebuild_history_popup();
	String _get_chat_display_name(const String &p_id) const;
	static String _format_relative_time(uint64_t p_unix_time);
	void _switch_to_chat(const String &p_id);
	void _delete_chat(const String &p_id);
	void _on_delete_chat_confirmed();

	// Event handlers
	void _on_send_button_pressed();  // Handles both Send and Stop
	void _on_prompt_text_changed();
	void _on_prompt_gui_input(const Ref<InputEvent> &p_event);
	void _on_ai_response(bool p_success, const String &p_response, const String &p_error);

	// Codex harness loop (experimental replacement for the orchestrator;
	// toggled via the model dropdown's "Codex Harness" entry). The driver
	// re-emits the orchestrator signal contract, so it shares the callbacks
	// below.
	Ref<CodexHarnessDriver> harness_driver;
	bool use_harness_mode = false;
	void _ensure_harness_driver();

	// Streaming state: the pending "Thinking..." label doubles as the live
	// stream target; it reverts to the dots between message items.
	String harness_stream_text;
	bool harness_streaming = false;
	Control *harness_stream_block = nullptr;
	RichTextLabel *harness_stream_rich = nullptr;
	void _on_harness_assistant_delta(const String &p_delta);
	void _reset_harness_stream();

	// Approval policy (Shift+Tab cycles Ask -> Auto -> Read-only; the driver
	// is the policy engine, this is just the UI + persistence).
	int harness_policy_mode = 0;
	Label *policy_mode_label = nullptr;
	void _cycle_policy_mode();
	void _update_policy_mode_label();

	// Approval prompt REPLACES the composer while pending (modern-CLI style;
	// the transcript stays clean — the action's tool card is its record).
	HBoxContainer *input_bar = nullptr;
	Control *approval_panel = nullptr;
	Label *approval_header = nullptr;
	RichTextLabel *approval_body = nullptr;
	Vector<Dictionary> approval_queue;
	void _build_approval_panel();
	void _show_next_approval();
	void _on_harness_approval_requested(const Dictionary &p_info);
	void _on_approval_decision(const String &p_decision);

	// Thinking stream: italic, lighter, one point smaller than body text.
	// Finalized blocks stay in the transcript as plain styled text.
	Control *harness_thinking_block = nullptr;
	RichTextLabel *harness_thinking_label = nullptr;
	String harness_thinking_text;
	Control *_create_thinking_block(RichTextLabel **r_label);
	// Shared assistant-text renderer (live finalization AND chat reload):
	// borderless rich text with markdown rendering — the single source of
	// truth for how assistant prose looks.
	Control *_create_assistant_text_block(const String &p_text, RichTextLabel **r_label = nullptr);
	void _on_harness_thinking_delta(const String &p_delta);
	void _finalize_harness_thinking();

	// Agentic orchestrator callbacks
	void _on_orchestrator_started();
	void _on_orchestrator_progress(const String &p_status, int p_turn);
	void _on_orchestrator_assistant_item(const Dictionary &p_item);
	void _on_orchestrator_tool_result(const Dictionary &p_tool_result);
	void _on_orchestrator_complete(bool p_success, const String &p_final_message);
	// Queued mid-run messages were just delivered to the model: persist them
	// to the transcript at their true position and drop their queue chips.
	void _on_user_injection_consumed(const Array &p_ids);
	void _on_orchestrator_narration(const String &p_text); // legacy no-op
	void _on_orchestrator_thinking(const String &p_text); // legacy no-op
	void _on_todos_updated(const Array &p_todos);
	void _on_api_round_started(int p_turn);
	void _on_turn_tokens_ready(int p_tokens);
	void _on_scene_diff_ready(const Dictionary &p_diff_info);

	// Pending message helpers
	void _show_pending_message();
	void _remove_pending_message();
	void _on_thinking_dot_tick();

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

	// Provider switching
	void _on_provider_changed(int p_index);

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
