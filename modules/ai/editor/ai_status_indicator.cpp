/**************************************************************************/
/*  ai_status_indicator.cpp                                               */
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

#include "ai_status_indicator.h"

#include "../ai.h"
#include "core/config/engine.h"
#include "core/input/input_event.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "core/string/ustring.h"
#include "editor/editor_node.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/rich_text_label.h"
#include "scene/resources/style_box_flat.h"
#include "scene/resources/font.h"
#include "scene/scene_string_names.h"

// Truncation limits for conversation context (must match ai_provider.cpp)
static const int MAX_CONTEXT_MESSAGES = 80;
static const int MAX_CONTEXT_CHARS = 120000; // 120k chars

// ============================================================================
// Cursor-like Dark Theme Color Palette
// ============================================================================
namespace AIColors {
	// Background hierarchy (near-true-black)
	static const Color BG_0 = Color(0.08, 0.08, 0.09, 1.0);      // #141417 - Main background
	static const Color BG_1 = Color(0.11, 0.11, 0.13, 1.0);      // #1C1C21 - Cards/tool entries
	static const Color BG_2 = Color(0.14, 0.14, 0.16, 1.0);      // #242428 - Input fields/buttons
	static const Color BG_3 = Color(0.18, 0.18, 0.20, 1.0);      // #2E2E33 - Hover states

	// Borders
	static const Color BORDER = Color(0.25, 0.25, 0.28, 1.0);    // #404047 - Subtle borders
	static const Color BORDER_LIGHT = Color(0.35, 0.35, 0.38, 1.0); // #595961 - Focus/hover borders

	// Text
	static const Color TEXT_PRIMARY = Color(0.93, 0.93, 0.95, 1.0);  // #EDEFF2 - Primary text
	static const Color TEXT_SECONDARY = Color(0.7, 0.7, 0.73, 1.0); // #B3B3BA - Secondary text
	static const Color TEXT_MUTED = Color(0.5, 0.5, 0.53, 1.0);     // #808087 - Muted/placeholder
	static const Color TEXT_DISABLED = Color(0.35, 0.35, 0.38, 1.0); // #595961 - Disabled text

	// Accent - Blue
	static const Color ACCENT_BLUE = Color(0.30, 0.52, 0.90, 1.0);   // #4D85E6 - Primary accent
	static const Color ACCENT_BLUE_HOVER = Color(0.35, 0.57, 0.95, 1.0); // #5991F2 - Hover
	static const Color ACCENT_BLUE_PRESSED = Color(0.25, 0.45, 0.80, 1.0); // #4073CC - Pressed
	static const Color ACCENT_BLUE_MUTED = Color(0.20, 0.32, 0.55, 0.95); // #33528C - User bubble

	// Assistant bubble - slightly warmer/distinct
	static const Color ASSISTANT_BG = Color(0.13, 0.14, 0.17, 1.0); // #21242B - Assistant messages

	// Status
	static const Color SUCCESS = Color(0.35, 0.78, 0.45, 1.0);   // #59C773 - Green
	static const Color ERROR = Color(0.90, 0.40, 0.40, 1.0);     // #E66666 - Red
	static const Color WARNING = Color(0.95, 0.75, 0.25, 1.0);   // #F2BF40 - Yellow/Orange

	// Spacing
	static const int CORNER_RADIUS_SM = 3;
	static const int CORNER_RADIUS_MD = 5;
	static const int CORNER_RADIUS_LG = 8;
	static const int PADDING_XS = 2;
	static const int PADDING_SM = 6;
	static const int PADDING_MD = 10;
	static const int PADDING_LG = 14;
}

// ============================================================================
// ToolCollapsibleEntry - Collapsible widget for tool results
// ============================================================================

void ToolCollapsibleEntry::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_on_toggle_pressed"), &ToolCollapsibleEntry::_on_toggle_pressed);
	ClassDB::bind_method(D_METHOD("_on_header_gui_input", "event"), &ToolCollapsibleEntry::_on_header_gui_input);
}

void ToolCollapsibleEntry::_on_toggle_pressed() {
	set_collapsed(!is_collapsed);
}

void ToolCollapsibleEntry::_on_header_gui_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MouseButton::LEFT) {
		set_collapsed(!is_collapsed);
	}
}

void ToolCollapsibleEntry::_update_toggle_icon() {
	if (toggle_button) {
		// Use unicode arrows: ▶ (collapsed) and ▼ (expanded)
		toggle_button->set_text(is_collapsed ? String::utf8("▶") : String::utf8("▼"));
	}
}

void ToolCollapsibleEntry::set_collapsed(bool p_collapsed) {
	if (is_collapsed == p_collapsed) {
		return;
	}

	is_collapsed = p_collapsed;
	_update_toggle_icon();

	if (body_container) {
		body_container->set_visible(!is_collapsed);
	}
}

bool ToolCollapsibleEntry::get_collapsed() const {
	return is_collapsed;
}

void ToolCollapsibleEntry::set_header(const String &p_text, const String &p_status) {
	if (header_label) {
		header_label->set_text(p_text);
	}
	if (status_label) {
		status_label->set_text(p_status);
		status_label->set_visible(!p_status.is_empty());
	}
}

void ToolCollapsibleEntry::set_body(const String &p_text) {
	if (body_text) {
		body_text->set_text(p_text);
		// Auto-size height based on content (approximate lines * line height)
		int line_count = p_text.get_slice_count("\n");
		int min_height = MAX(60, (line_count + 1) * 18) * EDSCALE;
		body_text->set_custom_minimum_size(Size2(0, min_height));
	}
}

void ToolCollapsibleEntry::update_from_tool_result(const Dictionary &p_tool_result) {
	String action_type = p_tool_result.get("type", "unknown");
	String status = p_tool_result.get("status", "unknown");

	// Build header text
	String header_text = vformat("[Tool] %s", action_type);

	// Build status indicator
	String status_text;
	if (status == "success") {
		status_text = String::utf8("✓");
	} else if (status == "error") {
		status_text = String::utf8("✗");
	}

	set_header(header_text, status_text);

	// Build body text with full details
	String body_content;
	if (status == "success") {
		body_content = "Status: Success";
		if (p_tool_result.has("result")) {
			Dictionary result = p_tool_result["result"];
			if (!result.is_empty()) {
				body_content += vformat("\nResult:\n%s", JSON::stringify(result, "  ", false));
			}
		}
	} else if (status == "error") {
		body_content = "Status: Error";
		if (p_tool_result.has("error")) {
			Dictionary error = p_tool_result["error"];
			String error_msg = error.get("message", "Unknown error");
			String error_code = error.get("code", "");
			if (!error_code.is_empty()) {
				body_content += vformat("\nCode: %s", error_code);
			}
			body_content += vformat("\nMessage: %s", error_msg);
		}
	} else {
		body_content = vformat("Status: %s", status);
	}

	// Add args if available
	if (p_tool_result.has("args") && p_tool_result["args"].get_type() == Variant::DICTIONARY) {
		Dictionary args = p_tool_result["args"];
		if (!args.is_empty()) {
			body_content += vformat("\nArgs: %s", JSON::stringify(args, "  ", false));
		}
	}

	set_body(body_content);

	// Update status label color
	if (status_label) {
		if (status == "success") {
			status_label->add_theme_color_override("font_color", AIColors::SUCCESS);
		} else if (status == "error") {
			status_label->add_theme_color_override("font_color", AIColors::ERROR);
		}
	}
}

ToolCollapsibleEntry::ToolCollapsibleEntry() {
	set_h_size_flags(SIZE_EXPAND_FILL);

	// Create main panel with background
	PanelContainer *main_panel = memnew(PanelContainer);
	main_panel->set_h_size_flags(SIZE_EXPAND_FILL);
	add_child(main_panel);

	// Style the panel with subtle border
	Ref<StyleBoxFlat> panel_style;
	panel_style.instantiate();
	panel_style->set_bg_color(AIColors::BG_1);
	panel_style->set_border_width_all(1);
	panel_style->set_border_color(AIColors::BORDER);
	panel_style->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
	panel_style->set_corner_radius_all(AIColors::CORNER_RADIUS_MD * EDSCALE);
	main_panel->add_theme_style_override("panel", panel_style);

	// Inner VBox for header and body
	VBoxContainer *inner_vbox = memnew(VBoxContainer);
	inner_vbox->set_h_size_flags(SIZE_EXPAND_FILL);
	inner_vbox->add_theme_constant_override("separation", AIColors::PADDING_XS * EDSCALE);
	main_panel->add_child(inner_vbox);

	// Header row
	header_container = memnew(HBoxContainer);
	header_container->set_h_size_flags(SIZE_EXPAND_FILL);
	header_container->add_theme_constant_override("separation", AIColors::PADDING_SM * EDSCALE);
	header_container->set_mouse_filter(MOUSE_FILTER_STOP);
	header_container->connect("gui_input", callable_mp(this, &ToolCollapsibleEntry::_on_header_gui_input));
	inner_vbox->add_child(header_container);

	// Toggle button (chevron) - styled flat button
	toggle_button = memnew(Button);
	toggle_button->set_flat(true);
	toggle_button->set_text(String::utf8("▶")); // Right-pointing triangle (collapsed)
	toggle_button->set_custom_minimum_size(Size2(20 * EDSCALE, 0));
	toggle_button->add_theme_color_override("font_color", AIColors::TEXT_MUTED);
	toggle_button->add_theme_color_override("font_hover_color", AIColors::TEXT_PRIMARY);
	toggle_button->add_theme_color_override("font_pressed_color", AIColors::TEXT_PRIMARY);
	toggle_button->connect(SceneStringNames::get_singleton()->pressed, callable_mp(this, &ToolCollapsibleEntry::_on_toggle_pressed));
	header_container->add_child(toggle_button);

	// Header label
	header_label = memnew(Label);
	header_label->set_h_size_flags(SIZE_EXPAND_FILL);
	header_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	header_label->add_theme_color_override("font_color", AIColors::TEXT_PRIMARY);
	header_container->add_child(header_label);

	// Status label (emoji indicator)
	status_label = memnew(Label);
	status_label->set_visible(false);
	header_container->add_child(status_label);

	// Body container (hidden by default)
	body_container = memnew(PanelContainer);
	body_container->set_h_size_flags(SIZE_EXPAND_FILL);
	body_container->set_visible(false); // Collapsed by default
	inner_vbox->add_child(body_container);

	// Style the body container
	Ref<StyleBoxFlat> body_style;
	body_style.instantiate();
	body_style->set_bg_color(AIColors::BG_0);
	body_style->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
	body_style->set_corner_radius_all(AIColors::CORNER_RADIUS_SM * EDSCALE);
	body_container->add_theme_style_override("panel", body_style);

	// Body text (read-only TextEdit for proper indentation handling)
	body_text = memnew(TextEdit);
	body_text->set_h_size_flags(SIZE_EXPAND_FILL);
	body_text->set_editable(false);
	body_text->set_context_menu_enabled(false);
	body_text->set_shortcut_keys_enabled(false);
	body_text->set_selecting_enabled(true);
	body_text->set_line_wrapping_mode(TextEdit::LINE_WRAPPING_BOUNDARY);
	body_text->set_custom_minimum_size(Size2(0, 60 * EDSCALE));
	body_text->add_theme_color_override("font_color", AIColors::TEXT_SECONDARY);
	body_text->add_theme_color_override("background_color", AIColors::BG_0);
	// Use a flat style for seamless look
	Ref<StyleBoxFlat> text_style;
	text_style.instantiate();
	text_style->set_bg_color(AIColors::BG_0);
	text_style->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
	body_text->add_theme_style_override("normal", text_style);
	body_text->add_theme_style_override("read_only", text_style);
	body_container->add_child(body_text);
}

// ============================================================================
// AIStatusIndicator - The colored circle showing connection status
// ============================================================================

void AIStatusIndicator::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_DRAW: {
			// Draw a filled circle
			Color indicator_color;
			switch (current_status) {
				case STATUS_UNKNOWN:
					indicator_color = AIColors::TEXT_MUTED;
					break;
				case STATUS_CHECKING:
					indicator_color = AIColors::WARNING;
					break;
				case STATUS_CONNECTED:
					indicator_color = AIColors::SUCCESS;
					break;
				case STATUS_DISCONNECTED:
					indicator_color = AIColors::ERROR;
					break;
			}

			Vector2 center = get_size() / 2.0;
			float radius = MIN(center.x, center.y) - 2.0;
			draw_circle(center, radius, indicator_color);
		} break;
	}
}

void AIStatusIndicator::_bind_methods() {
	BIND_ENUM_CONSTANT(STATUS_UNKNOWN);
	BIND_ENUM_CONSTANT(STATUS_CHECKING);
	BIND_ENUM_CONSTANT(STATUS_CONNECTED);
	BIND_ENUM_CONSTANT(STATUS_DISCONNECTED);
}

void AIStatusIndicator::set_status(Status p_status) {
	if (current_status != p_status) {
		current_status = p_status;
		queue_redraw();
	}
}

AIStatusIndicator::Status AIStatusIndicator::get_status() const {
	return current_status;
}

AIStatusIndicator::AIStatusIndicator() {
	set_custom_minimum_size(Size2(12, 12) * EDSCALE);
	set_mouse_filter(MOUSE_FILTER_PASS);
	set_v_size_flags(SIZE_SHRINK_CENTER);
}

// ============================================================================
// AIStatusPanel - The chat panel containing transcript and input
// ============================================================================

void AIStatusPanel::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			// Load transcript and rebuild UI
			if (chat_store.is_valid()) {
				chat_store->load_transcript();
				_rebuild_message_list();
			}
			// Initial connectivity check
			check_api_connectivity();

			// Connect to AI provider signal
			if (Engine::get_singleton()->has_singleton("AI")) {
				Object *ai_obj = Engine::get_singleton()->get_singleton_object("AI");
				AI *ai = Object::cast_to<AI>(ai_obj);
				if (ai) {
					Ref<AIProvider> provider = ai->get_provider();
					if (provider.is_valid()) {
						if (!provider->is_connected("request_completed", callable_mp(this, &AIStatusPanel::_on_ai_response))) {
							provider->connect("request_completed", callable_mp(this, &AIStatusPanel::_on_ai_response));
						}
					}

					// Connect to orchestrator signals for agentic tool use
					Ref<AgenticOrchestrator> orchestrator = ai->get_orchestrator();
					if (orchestrator.is_valid()) {
						if (!orchestrator->is_connected("progress_update", callable_mp(this, &AIStatusPanel::_on_orchestrator_progress))) {
							orchestrator->connect("progress_update", callable_mp(this, &AIStatusPanel::_on_orchestrator_progress));
						}
						if (!orchestrator->is_connected("tool_result_ready", callable_mp(this, &AIStatusPanel::_on_orchestrator_tool_result))) {
							orchestrator->connect("tool_result_ready", callable_mp(this, &AIStatusPanel::_on_orchestrator_tool_result));
						}
						if (!orchestrator->is_connected("run_complete", callable_mp(this, &AIStatusPanel::_on_orchestrator_complete))) {
							orchestrator->connect("run_complete", callable_mp(this, &AIStatusPanel::_on_orchestrator_complete));
						}
					}
				}
			}
		} break;

		case NOTIFICATION_THEME_CHANGED: {
			// Update button icons if needed
			_rebuild_message_list();
		} break;
	}
}

void AIStatusPanel::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_on_send_button_pressed"), &AIStatusPanel::_on_send_button_pressed);
	ClassDB::bind_method(D_METHOD("_on_clear_pressed"), &AIStatusPanel::_on_clear_pressed);
	ClassDB::bind_method(D_METHOD("_on_prompt_text_changed"), &AIStatusPanel::_on_prompt_text_changed);
	ClassDB::bind_method(D_METHOD("_on_ai_response", "success", "response", "error"), &AIStatusPanel::_on_ai_response);
	ClassDB::bind_method(D_METHOD("_on_orchestrator_progress", "status", "turn"), &AIStatusPanel::_on_orchestrator_progress);
	ClassDB::bind_method(D_METHOD("_on_orchestrator_tool_result", "tool_result"), &AIStatusPanel::_on_orchestrator_tool_result);
	ClassDB::bind_method(D_METHOD("_on_orchestrator_complete", "success", "final_message"), &AIStatusPanel::_on_orchestrator_complete);
	ClassDB::bind_method(D_METHOD("_on_openai_request_completed", "result", "response_code", "headers", "body"), &AIStatusPanel::_on_openai_request_completed);
	ClassDB::bind_method(D_METHOD("_on_gemini_request_completed", "result", "response_code", "headers", "body"), &AIStatusPanel::_on_gemini_request_completed);
	ClassDB::bind_method(D_METHOD("_on_xai_request_completed", "result", "response_code", "headers", "body"), &AIStatusPanel::_on_xai_request_completed);
	ClassDB::bind_method(D_METHOD("check_api_connectivity"), &AIStatusPanel::check_api_connectivity);
}

void AIStatusPanel::_rebuild_message_list() {
	if (!message_list) {
		return;
	}

	// Clear existing messages
	while (message_list->get_child_count() > 0) {
		Node *child = message_list->get_child(0);
		message_list->remove_child(child);
		memdelete(child);
	}
	pending_message = nullptr;

	// Add all messages from store
	if (chat_store.is_valid()) {
		const Vector<ChatMessage> &messages = chat_store->get_messages();
		for (int i = 0; i < messages.size(); i++) {
			const ChatMessage &msg = messages[i];
			Control *ui_element = nullptr;

			if (msg.role == "tool") {
				// Tool results: parse JSON content and create collapsible entry
				JSON json;
				Error err = json.parse(msg.content);
				if (err == OK && json.get_data().get_type() == Variant::DICTIONARY) {
					Dictionary tool_result = json.get_data();
					ui_element = _create_tool_result_ui(tool_result);
				} else {
					// Fallback: render as regular message if JSON parse fails
					ui_element = _create_message_bubble(msg);
				}
			} else {
				// User/Assistant messages: render as bubbles
				ui_element = _create_message_bubble(msg);
			}

			if (ui_element) {
				message_list->add_child(ui_element);
			}
		}
	}

	// Scroll to bottom after rebuild
	callable_mp(this, &AIStatusPanel::_scroll_to_bottom).call_deferred();
}

void AIStatusPanel::_append_message_ui(const ChatMessage &p_message) {
	if (!message_list) {
		return;
	}

	Control *bubble = _create_message_bubble(p_message);
	if (bubble) {
		message_list->add_child(bubble);
		callable_mp(this, &AIStatusPanel::_scroll_to_bottom).call_deferred();
	}
}

Control *AIStatusPanel::_create_message_bubble(const ChatMessage &p_message) {
	// Create container for alignment
	HBoxContainer *align_container = memnew(HBoxContainer);
	align_container->set_h_size_flags(SIZE_EXPAND_FILL);

	// Create the bubble panel
	PanelContainer *bubble = memnew(PanelContainer);

	// Style the bubble based on role
	Ref<StyleBoxFlat> style;
	style.instantiate();
	style->set_corner_radius_all(AIColors::CORNER_RADIUS_LG * EDSCALE);
	style->set_content_margin_all(AIColors::PADDING_MD * EDSCALE);

	bool is_user = p_message.role == "user";

	if (is_user) {
		// User messages: blue accent, right aligned (no border)
		style->set_bg_color(AIColors::ACCENT_BLUE_MUTED);
		style->set_border_width_all(0); // Explicitly no border
		// Add flexible spacer on left to push bubble right
		Control *spacer = memnew(Control);
		spacer->set_h_size_flags(SIZE_EXPAND_FILL);
		spacer->set_stretch_ratio(0.2); // Take up to 20% of space
		align_container->add_child(spacer);
		bubble->set_h_size_flags(SIZE_EXPAND_FILL);
		bubble->set_stretch_ratio(0.8); // Bubble takes up to 80%
		align_container->add_child(bubble);
	} else {
		// Assistant messages: distinct dark with subtle border, left aligned
		style->set_bg_color(AIColors::ASSISTANT_BG);
		style->set_border_width_all(1);
		style->set_border_color(AIColors::BORDER);
		bubble->set_h_size_flags(SIZE_EXPAND_FILL);
		bubble->set_stretch_ratio(0.95); // Bubble takes up to 95%
		align_container->add_child(bubble);
		// Add small spacer on right
		Control *spacer = memnew(Control);
		spacer->set_h_size_flags(SIZE_EXPAND_FILL);
		spacer->set_stretch_ratio(0.05);
		align_container->add_child(spacer);
	}

	bubble->add_theme_style_override("panel", style);

	// Create label for content
	RichTextLabel *label = memnew(RichTextLabel);
	label->set_use_bbcode(true);
	label->set_fit_content(true);
	label->set_scroll_active(false);
	label->set_selection_enabled(true);
	label->add_theme_color_override("default_color", AIColors::TEXT_PRIMARY);

	// Display content
	label->add_text(p_message.content);

	bubble->add_child(label);

	return align_container;
}

Control *AIStatusPanel::_create_tool_result_ui(const Dictionary &p_tool_result) {
	// Create collapsible entry for tool result (collapsed by default)
	ToolCollapsibleEntry *entry = memnew(ToolCollapsibleEntry);
	entry->update_from_tool_result(p_tool_result);
	return entry;
}

void AIStatusPanel::_append_tool_result_ui(const Dictionary &p_tool_result) {
	if (!message_list) {
		return;
	}

	Control *tool_result_ui = _create_tool_result_ui(p_tool_result);
	if (tool_result_ui) {
		message_list->add_child(tool_result_ui);
		callable_mp(this, &AIStatusPanel::_scroll_to_bottom).call_deferred();
	}
}

void AIStatusPanel::_scroll_to_bottom() {
	if (transcript_scroll) {
		transcript_scroll->set_v_scroll(transcript_scroll->get_v_scroll_bar()->get_max());
	}
}

void AIStatusPanel::_update_send_button_state() {
	if (!send_button || !prompt_edit) {
		return;
	}

	// Button appearance and behavior depends on run state
	switch (run_state) {
		case STATE_IDLE: {
			// Send mode: enabled if there's text to send
			send_button->set_text(TTR("Send"));
			bool has_text = !prompt_edit->get_text().strip_edges().is_empty();
			send_button->set_disabled(!has_text);

			// Apply accent blue style for Send
			Ref<StyleBoxFlat> send_normal;
			send_normal.instantiate();
			send_normal->set_bg_color(AIColors::ACCENT_BLUE);
			send_normal->set_corner_radius_all(AIColors::CORNER_RADIUS_MD * EDSCALE);
			send_normal->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
			send_button->add_theme_style_override("normal", send_normal);

			Ref<StyleBoxFlat> send_hover;
			send_hover.instantiate();
			send_hover->set_bg_color(AIColors::ACCENT_BLUE_HOVER);
			send_hover->set_corner_radius_all(AIColors::CORNER_RADIUS_MD * EDSCALE);
			send_hover->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
			send_button->add_theme_style_override("hover", send_hover);
		} break;

		case STATE_RUNNING: {
			// Stop mode: always enabled, shows Stop
			send_button->set_text(TTR("Stop"));
			send_button->set_disabled(false);

			// Apply warning/red style for Stop
			Ref<StyleBoxFlat> stop_normal;
			stop_normal.instantiate();
			stop_normal->set_bg_color(AIColors::ERROR.darkened(0.2));
			stop_normal->set_corner_radius_all(AIColors::CORNER_RADIUS_MD * EDSCALE);
			stop_normal->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
			send_button->add_theme_style_override("normal", stop_normal);

			Ref<StyleBoxFlat> stop_hover;
			stop_hover.instantiate();
			stop_hover->set_bg_color(AIColors::ERROR);
			stop_hover->set_corner_radius_all(AIColors::CORNER_RADIUS_MD * EDSCALE);
			stop_hover->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
			send_button->add_theme_style_override("hover", stop_hover);
		} break;

		case STATE_CANCELLING: {
			// Cancelling: disabled, shows Stopping...
			send_button->set_text(TTR("Stopping..."));
			send_button->set_disabled(true);
		} break;
	}
}

void AIStatusPanel::_update_queue_ui() {
	if (!queue_count_label) {
		return;
	}

	if (message_queue.is_empty()) {
		queue_count_label->set_visible(false);
	} else {
		queue_count_label->set_text(vformat(TTR("Queued: %d"), message_queue.size()));
		queue_count_label->set_visible(true);
	}
}

// ============================================================================
// Message Queue Management
// ============================================================================

String AIStatusPanel::_generate_queue_id() {
	static uint64_t counter = 0;
	return vformat("queue_%d_%d", OS::get_singleton()->get_ticks_msec(), counter++);
}

void AIStatusPanel::_enqueue_message(const String &p_text) {
	QueuedMessage msg;
	msg.id = _generate_queue_id();
	msg.text = p_text;
	msg.created_at = OS::get_singleton()->get_ticks_msec();
	message_queue.push_back(msg);

	print_line(vformat("AI Queue: Enqueued message (queue size: %d)", message_queue.size()));
	_update_queue_ui();
}

void AIStatusPanel::_dequeue_and_run_next() {
	if (message_queue.is_empty()) {
		return;
	}

	if (run_state != STATE_IDLE) {
		// Still running, don't dequeue
		return;
	}

	QueuedMessage next = message_queue[0];
	message_queue.remove_at(0);

	print_line(vformat("AI Queue: Dequeued message, %d remaining", message_queue.size()));
	_update_queue_ui();

	// Start the run with the dequeued message
	_start_run(next.text);
}

void AIStatusPanel::_remove_queued_message(int p_index) {
	if (p_index < 0 || p_index >= message_queue.size()) {
		return;
	}

	message_queue.remove_at(p_index);
	print_line(vformat("AI Queue: Removed message at index %d, %d remaining", p_index, message_queue.size()));
	_update_queue_ui();
}

// ============================================================================
// Run State Management
// ============================================================================

void AIStatusPanel::_set_run_state(RunState p_state) {
	if (run_state == p_state) {
		return;
	}

	run_state = p_state;
	print_line(vformat("AI Run State: %s", p_state == STATE_IDLE ? "IDLE" : (p_state == STATE_RUNNING ? "RUNNING" : "CANCELLING")));
	_update_send_button_state();
}

void AIStatusPanel::_start_run(const String &p_message) {
	if (run_state != STATE_IDLE) {
		// Already running - this shouldn't happen, but enqueue just in case
		_enqueue_message(p_message);
		return;
	}

	// Append user message to store
	if (chat_store.is_valid()) {
		ChatMessage user_msg = chat_store->append_message("user", p_message);
		_append_message_ui(user_msg);
	}

	// Show pending message
	_show_pending_message();

	// Update state
	_set_run_state(STATE_RUNNING);
	is_waiting_for_response = true;

	// Build full message history for context
	Array messages = _build_model_messages();

	// Send to AI via agentic orchestrator
	if (Engine::get_singleton()->has_singleton("AI")) {
		Object *ai_obj = Engine::get_singleton()->get_singleton_object("AI");
		AI *ai = Object::cast_to<AI>(ai_obj);
		if (ai) {
			Ref<AIProvider> provider = ai->get_provider();
			Ref<AgenticOrchestrator> orchestrator = ai->get_orchestrator();

			if (orchestrator.is_valid() && provider.is_valid()) {
				print_line(vformat("AI Chat Panel: Starting agentic run with %d messages", messages.size()));
				orchestrator->run_agentic_loop(messages, provider);
			} else {
				ERR_PRINT("AI Chat Panel: Orchestrator or provider not available.");
				_remove_pending_message();
				is_waiting_for_response = false;
				_set_run_state(STATE_IDLE);
			}
		}
	} else {
		ERR_PRINT("AI Chat Panel: AI singleton not found.");
		_remove_pending_message();
		is_waiting_for_response = false;
		_set_run_state(STATE_IDLE);
	}
}

void AIStatusPanel::_request_cancel() {
	if (run_state != STATE_RUNNING) {
		return;
	}

	_set_run_state(STATE_CANCELLING);

	// Cancel the current agentic run
	if (Engine::get_singleton()->has_singleton("AI")) {
		Object *ai_obj = Engine::get_singleton()->get_singleton_object("AI");
		AI *ai = Object::cast_to<AI>(ai_obj);
		if (ai) {
			Ref<AgenticOrchestrator> orchestrator = ai->get_orchestrator();
			if (orchestrator.is_valid() && orchestrator->is_running()) {
				print_line("AI Chat Panel: Cancelling agentic run");
				orchestrator->cancel_run();
				// UI will update when _on_orchestrator_complete is called
			}
		}
	}
}

Array AIStatusPanel::_build_model_messages() {
	Array messages;
	context_was_truncated = false;
	
	if (!chat_store.is_valid()) {
		return messages;
	}
	
	const Vector<ChatMessage> &transcript = chat_store->get_messages();
	
	if (transcript.is_empty()) {
		return messages;
	}
	
	// Calculate total chars and find truncation point
	// We work backwards from the most recent message to keep the latest context
	int total_chars = 0;
	int start_index = 0;
	
	for (int i = transcript.size() - 1; i >= 0; i--) {
		total_chars += transcript[i].content.length();
		int message_count = transcript.size() - i;
		
		if (total_chars > MAX_CONTEXT_CHARS || message_count > MAX_CONTEXT_MESSAGES) {
			start_index = i + 1;
			context_was_truncated = true;
			WARN_PRINT(vformat("AI: Context truncated. Using %d of %d messages (%d chars).", 
				transcript.size() - start_index, transcript.size(), total_chars - transcript[i].content.length()));
			break;
		}
	}
	
	// Build messages array from start_index
	for (int i = start_index; i < transcript.size(); i++) {
		Dictionary msg;
		msg["role"] = transcript[i].role;
		msg["content"] = transcript[i].content;
		messages.push_back(msg);
	}
	
	// Log context info
	int final_chars = 0;
	for (int i = start_index; i < transcript.size(); i++) {
		final_chars += transcript[i].content.length();
	}
	print_line(vformat("AI: Built %d messages for context (%d chars)%s", 
		messages.size(), final_chars, context_was_truncated ? " [TRUNCATED]" : ""));
	
	return messages;
}

void AIStatusPanel::_on_send_button_pressed() {
	// Button behavior depends on current run state
	switch (run_state) {
		case STATE_IDLE: {
			// Send mode: try to send the message
			if (!prompt_edit) {
				return;
			}

			String prompt_text = prompt_edit->get_text().strip_edges();
			if (prompt_text.is_empty()) {
				return;
			}

			// Clear input immediately for better UX
			prompt_edit->set_text("");
			_update_send_button_state();

			// Start the run
			_start_run(prompt_text);
		} break;

		case STATE_RUNNING: {
			// Stop mode: request cancellation
			_request_cancel();
		} break;

		case STATE_CANCELLING: {
			// Already cancelling, ignore
		} break;
	}
}

void AIStatusPanel::_on_clear_pressed() {
	if (chat_store.is_valid()) {
		chat_store->clear_transcript();
	}
	_rebuild_message_list();
}

void AIStatusPanel::_on_prompt_text_changed() {
	_update_send_button_state();
}

void AIStatusPanel::_on_prompt_gui_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventKey> key_event = p_event;
	if (key_event.is_valid() && key_event->is_pressed()) {
		if (key_event->get_keycode() == Key::ENTER) {
			if (key_event->is_shift_pressed()) {
				// Shift+Enter: insert newline explicitly
				prompt_edit->accept_event();
				prompt_edit->insert_text_at_caret("\n");
			} else {
				// Enter without Shift: send the message
				// Skip if IME composition is active (best-effort for non-English input)
				if (prompt_edit->has_ime_text()) {
					return;
				}

				String prompt_text = prompt_edit->get_text().strip_edges();
				if (prompt_text.is_empty()) {
					return;
				}

				// Accept the event to prevent newline insertion
				prompt_edit->accept_event();

				// If running, queue the message instead of trying to send
				if (run_state != STATE_IDLE) {
					prompt_edit->set_text("");
					_enqueue_message(prompt_text);
					_update_send_button_state();
				} else {
					// Idle: trigger normal send
					_on_send_button_pressed();
				}
			}
		}
	}
}

void AIStatusPanel::_on_ai_response(bool p_success, const String &p_response, const String &p_error) {
	// If orchestrator is running, it handles responses via its own callbacks - skip this legacy handler
	if (Engine::get_singleton()->has_singleton("AI")) {
		Object *ai_obj = Engine::get_singleton()->get_singleton_object("AI");
		AI *ai = Object::cast_to<AI>(ai_obj);
		if (ai) {
			Ref<AgenticOrchestrator> orchestrator = ai->get_orchestrator();
			if (orchestrator.is_valid() && orchestrator->is_running()) {
				print_verbose("AIStatusPanel: Skipping legacy _on_ai_response - orchestrator is running");
				return;
			}
		}
	}

	// Remove pending message
	_remove_pending_message();

	// Append assistant response
	String content;
	if (p_success) {
		// Try to parse the new format: {"message": "...", "actions": [...]}
		JSON json;
		Error err = json.parse(p_response);
		if (err == OK && json.get_data().get_type() == Variant::DICTIONARY) {
			Dictionary response_dict = json.get_data();
			String message = response_dict.get("message", "");
			Array actions = response_dict.get("actions", Array());
			
			// Build display content
			if (!message.is_empty()) {
				content = message;
			}
			
			// If there are actions, append a summary
			if (actions.size() > 0) {
				if (!content.is_empty()) {
					content += "\n\n";
				}
				content += vformat("[%d action(s) executed]", actions.size());
			}
			
			// Fallback if both are empty
			if (content.is_empty()) {
				content = "(No response)";
			}
		} else {
			// Legacy format or parse error - show raw response
			content = p_response;
		}
	} else {
		content = vformat("[Error] %s", p_error);
	}

	if (chat_store.is_valid()) {
		ChatMessage assistant_msg = chat_store->append_message("assistant", content);
		_append_message_ui(assistant_msg);
	}

	// Update state
	is_waiting_for_response = false;
	_set_run_state(STATE_IDLE);
}

void AIStatusPanel::_on_orchestrator_progress(const String &p_status, int p_turn) {
	// Update status label with progress
	if (status_label) {
		status_label->set_text(vformat("Turn %d: %s", p_turn, p_status));
	}
	print_verbose(vformat("AI Chat Panel: Agentic progress (turn %d): %s", p_turn, p_status));
}

void AIStatusPanel::_on_orchestrator_tool_result(const Dictionary &p_tool_result) {
	print_line(vformat("AIStatusPanel: _on_orchestrator_tool_result called - type=%s, status=%s",
		String(p_tool_result.get("type", "unknown")), String(p_tool_result.get("status", "unknown"))));

	// Append tool result to chat transcript
	_append_tool_result_ui(p_tool_result);

	// Also append to chat store for persistence
	if (chat_store.is_valid()) {
		chat_store->append_tool_result(p_tool_result);
	}
}

void AIStatusPanel::_on_orchestrator_complete(bool p_success, const String &p_final_message) {
	print_line(vformat("AIStatusPanel: _on_orchestrator_complete called - success=%s, message_length=%d", p_success ? "true" : "false", p_final_message.length()));

	// Remove pending message
	_remove_pending_message();

	// Append final message to chat
	if (chat_store.is_valid() && !p_final_message.is_empty()) {
		print_line("AIStatusPanel: Appending final assistant message to chat");
		ChatMessage assistant_msg = chat_store->append_message("assistant", p_final_message);
		_append_message_ui(assistant_msg);
	} else {
		print_line(vformat("AIStatusPanel: Not appending message - chat_store valid=%s, message empty=%s",
			chat_store.is_valid() ? "true" : "false", p_final_message.is_empty() ? "true" : "false"));
	}

	// Update state
	is_waiting_for_response = false;
	_set_run_state(STATE_IDLE);

	// Reset status label
	if (status_label) {
		status_label->set_text(p_success ? TTR("Ready") : TTR("Cancelled"));
	}

	// Check for queued messages and process the next one
	if (!message_queue.is_empty()) {
		print_line(vformat("AIStatusPanel: Run complete, %d messages in queue. Starting next...", message_queue.size()));
		// Use call_deferred to avoid re-entrancy issues
		callable_mp(this, &AIStatusPanel::_dequeue_and_run_next).call_deferred();
	}
}

void AIStatusPanel::_show_pending_message() {
	if (!message_list || pending_message) {
		return;
	}

	// Create a "thinking" message
	ChatMessage thinking_msg;
	thinking_msg.role = "assistant";
	thinking_msg.content = "Assistant is thinking...";

	pending_message = _create_message_bubble(thinking_msg);
	if (pending_message) {
		message_list->add_child(pending_message);
		callable_mp(this, &AIStatusPanel::_scroll_to_bottom).call_deferred();
	}
}

void AIStatusPanel::_remove_pending_message() {
	if (pending_message && message_list) {
		message_list->remove_child(pending_message);
		memdelete(pending_message);
		pending_message = nullptr;
	}
}

void AIStatusPanel::check_api_connectivity() {
	// Guard: skip if checks are already in progress
	if (pending_checks > 0) {
		print_verbose("AI Status: Connectivity check already in progress, skipping");
		return;
	}

	// Reset state
	openai_connected = false;
	gemini_connected = false;
	xai_connected = false;
	pending_checks = 3;

	if (status_indicator) {
		status_indicator->set_status(AIStatusIndicator::STATUS_CHECKING);
		status_indicator->set_tooltip_text(TTR("Checking API connectivity..."));
	}

	if (status_label) {
		status_label->set_text(TTR("Checking..."));
	}

	// Send requests to check each provider
	// OpenAI: GET https://api.openai.com/v1/models
	if (http_openai) {
		Error err = http_openai->request("https://api.openai.com/v1/models", PackedStringArray(), HTTPClient::METHOD_GET);
		if (err != OK) {
			pending_checks--;
			print_verbose("AI Status: Failed to send OpenAI check request");
		}
	}

	// Gemini: GET https://generativelanguage.googleapis.com/v1beta/models
	if (http_gemini) {
		Error err = http_gemini->request("https://generativelanguage.googleapis.com/v1beta/models", PackedStringArray(), HTTPClient::METHOD_GET);
		if (err != OK) {
			pending_checks--;
			print_verbose("AI Status: Failed to send Gemini check request");
		}
	}

	// x.ai: GET https://api.x.ai/v1/models
	if (http_xai) {
		Error err = http_xai->request("https://api.x.ai/v1/models", PackedStringArray(), HTTPClient::METHOD_GET);
		if (err != OK) {
			pending_checks--;
			print_verbose("AI Status: Failed to send x.ai check request");
		}
	}

	// If all requests failed to send, update status immediately
	if (pending_checks == 0) {
		_update_status_from_results();
	}
}

void AIStatusPanel::_on_openai_request_completed(int p_result, int p_code, const PackedStringArray &p_headers, const PackedByteArray &p_body) {
	// Consider connected if we got any HTTP response (even 401 means the server is reachable)
	if (p_result == HTTPRequest::RESULT_SUCCESS && p_code > 0) {
		openai_connected = true;
		print_verbose(vformat("AI Status: OpenAI endpoint reachable (HTTP %d)", p_code));
	} else {
		print_verbose(vformat("AI Status: OpenAI endpoint unreachable (result: %d)", p_result));
	}

	pending_checks--;
	if (pending_checks <= 0) {
		_update_status_from_results();
	}
}

void AIStatusPanel::_on_gemini_request_completed(int p_result, int p_code, const PackedStringArray &p_headers, const PackedByteArray &p_body) {
	if (p_result == HTTPRequest::RESULT_SUCCESS && p_code > 0) {
		gemini_connected = true;
		print_verbose(vformat("AI Status: Gemini endpoint reachable (HTTP %d)", p_code));
	} else {
		print_verbose(vformat("AI Status: Gemini endpoint unreachable (result: %d)", p_result));
	}

	pending_checks--;
	if (pending_checks <= 0) {
		_update_status_from_results();
	}
}

void AIStatusPanel::_on_xai_request_completed(int p_result, int p_code, const PackedStringArray &p_headers, const PackedByteArray &p_body) {
	if (p_result == HTTPRequest::RESULT_SUCCESS && p_code > 0) {
		xai_connected = true;
		print_verbose(vformat("AI Status: x.ai endpoint reachable (HTTP %d)", p_code));
	} else {
		print_verbose(vformat("AI Status: x.ai endpoint unreachable (result: %d)", p_result));
	}

	pending_checks--;
	if (pending_checks <= 0) {
		_update_status_from_results();
	}
}

void AIStatusPanel::_update_status_from_results() {
	bool any_connected = openai_connected || gemini_connected || xai_connected;

	if (status_indicator) {
		if (any_connected) {
			status_indicator->set_status(AIStatusIndicator::STATUS_CONNECTED);

			// Build tooltip showing which providers are available
			String tooltip = TTR("API Status: Connected") + "\n";
			if (openai_connected) {
				tooltip += "  • OpenAI: " + TTR("Reachable") + "\n";
			}
			if (gemini_connected) {
				tooltip += "  • Gemini: " + TTR("Reachable") + "\n";
			}
			if (xai_connected) {
				tooltip += "  • x.ai: " + TTR("Reachable") + "\n";
			}
			status_indicator->set_tooltip_text(tooltip.strip_edges());

			print_line("AI Status: Connected to at least one provider");
		} else {
			status_indicator->set_status(AIStatusIndicator::STATUS_DISCONNECTED);
			status_indicator->set_tooltip_text(TTR("API Status: No providers reachable\nCheck your internet connection."));
			print_line("AI Status: No providers reachable");
		}
	}

	if (status_label) {
		if (any_connected) {
			status_label->set_text(TTR("Connected"));
		} else {
			status_label->set_text(TTR("Disconnected"));
		}
	}
}

AIStatusPanel::AIStatusPanel() {
	set_name("AI");

	// Apply dark background to main panel
	Ref<StyleBoxFlat> panel_bg;
	panel_bg.instantiate();
	panel_bg->set_bg_color(AIColors::BG_0);
	panel_bg->set_content_margin(SIDE_LEFT, AIColors::PADDING_SM * EDSCALE);
	panel_bg->set_content_margin(SIDE_RIGHT, AIColors::PADDING_SM * EDSCALE);
	panel_bg->set_content_margin(SIDE_TOP, AIColors::PADDING_SM * EDSCALE);
	panel_bg->set_content_margin(SIDE_BOTTOM, AIColors::PADDING_XS * EDSCALE); // Minimal bottom padding
	add_theme_style_override("panel", panel_bg);

	// Initialize chat store
	chat_store.instantiate();

	// ========================================
	// Transcript scroll area (top, expandable)
	// ========================================
	transcript_scroll = memnew(ScrollContainer);
	transcript_scroll->set_h_size_flags(SIZE_EXPAND_FILL);
	transcript_scroll->set_v_size_flags(SIZE_EXPAND_FILL);
	transcript_scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	add_child(transcript_scroll);

	// Dark background for scroll container
	Ref<StyleBoxFlat> scroll_style;
	scroll_style.instantiate();
	scroll_style->set_bg_color(AIColors::BG_0);
	scroll_style->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
	transcript_scroll->add_theme_style_override("panel", scroll_style);

	message_list = memnew(VBoxContainer);
	message_list->set_h_size_flags(SIZE_EXPAND_FILL);
	message_list->add_theme_constant_override("separation", AIColors::PADDING_SM * EDSCALE);
	transcript_scroll->add_child(message_list);

	// ========================================
	// Separator - subtle divider line (compact)
	// ========================================
	HSeparator *separator = memnew(HSeparator);
	Ref<StyleBoxFlat> sep_style;
	sep_style.instantiate();
	sep_style->set_bg_color(AIColors::BORDER);
	sep_style->set_content_margin(SIDE_TOP, 2 * EDSCALE);
	sep_style->set_content_margin(SIDE_BOTTOM, 2 * EDSCALE);
	separator->add_theme_style_override("separator", sep_style);
	add_child(separator);

	// ========================================
	// Input bar (bottom)
	// ========================================
	HBoxContainer *input_bar = memnew(HBoxContainer);
	input_bar->add_theme_constant_override("separation", AIColors::PADDING_SM * EDSCALE);
	add_child(input_bar);

	// Prompt text edit
	prompt_edit = memnew(TextEdit);
	prompt_edit->set_placeholder(TTR("Type a message..."));
	prompt_edit->set_h_size_flags(SIZE_EXPAND_FILL);
	prompt_edit->set_custom_minimum_size(Size2(0, 60 * EDSCALE));
	prompt_edit->set_line_wrapping_mode(TextEdit::LINE_WRAPPING_BOUNDARY);
	prompt_edit->connect("text_changed", callable_mp(this, &AIStatusPanel::_on_prompt_text_changed));
	prompt_edit->connect("gui_input", callable_mp(this, &AIStatusPanel::_on_prompt_gui_input));

	// Dark input field styling - normal state
	Ref<StyleBoxFlat> prompt_normal;
	prompt_normal.instantiate();
	prompt_normal->set_bg_color(AIColors::BG_2);
	prompt_normal->set_border_width_all(1);
	prompt_normal->set_border_color(AIColors::BORDER);
	prompt_normal->set_corner_radius_all(AIColors::CORNER_RADIUS_MD * EDSCALE);
	prompt_normal->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
	prompt_edit->add_theme_style_override("normal", prompt_normal);

	// Focus state - highlighted border
	Ref<StyleBoxFlat> prompt_focus;
	prompt_focus.instantiate();
	prompt_focus->set_bg_color(AIColors::BG_2);
	prompt_focus->set_border_width_all(2);
	prompt_focus->set_border_color(AIColors::ACCENT_BLUE);
	prompt_focus->set_corner_radius_all(AIColors::CORNER_RADIUS_MD * EDSCALE);
	prompt_focus->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
	prompt_edit->add_theme_style_override("focus", prompt_focus);

	prompt_edit->add_theme_color_override("font_color", AIColors::TEXT_PRIMARY);
	prompt_edit->add_theme_color_override("font_placeholder_color", AIColors::TEXT_MUTED);
	prompt_edit->add_theme_color_override("caret_color", AIColors::ACCENT_BLUE);
	prompt_edit->add_theme_color_override("selection_color", AIColors::ACCENT_BLUE_MUTED);

	input_bar->add_child(prompt_edit);

	// Button column
	VBoxContainer *button_column = memnew(VBoxContainer);
	button_column->add_theme_constant_override("separation", AIColors::PADDING_XS * EDSCALE);
	input_bar->add_child(button_column);

	// === Send/Stop button (toggles based on run state) ===
	send_button = memnew(Button);
	send_button->set_text(TTR("Send"));
	send_button->set_disabled(true);
	send_button->connect(SceneStringNames::get_singleton()->pressed, callable_mp(this, &AIStatusPanel::_on_send_button_pressed));

	// Send button - normal state
	Ref<StyleBoxFlat> send_normal;
	send_normal.instantiate();
	send_normal->set_bg_color(AIColors::ACCENT_BLUE);
	send_normal->set_corner_radius_all(AIColors::CORNER_RADIUS_MD * EDSCALE);
	send_normal->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
	send_button->add_theme_style_override("normal", send_normal);

	// Send button - hover state
	Ref<StyleBoxFlat> send_hover;
	send_hover.instantiate();
	send_hover->set_bg_color(AIColors::ACCENT_BLUE_HOVER);
	send_hover->set_corner_radius_all(AIColors::CORNER_RADIUS_MD * EDSCALE);
	send_hover->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
	send_button->add_theme_style_override("hover", send_hover);

	// Send button - pressed state
	Ref<StyleBoxFlat> send_pressed;
	send_pressed.instantiate();
	send_pressed->set_bg_color(AIColors::ACCENT_BLUE_PRESSED);
	send_pressed->set_corner_radius_all(AIColors::CORNER_RADIUS_MD * EDSCALE);
	send_pressed->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
	send_button->add_theme_style_override("pressed", send_pressed);

	// Send button - disabled state
	Ref<StyleBoxFlat> send_disabled;
	send_disabled.instantiate();
	send_disabled->set_bg_color(AIColors::BG_2);
	send_disabled->set_corner_radius_all(AIColors::CORNER_RADIUS_MD * EDSCALE);
	send_disabled->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
	send_button->add_theme_style_override("disabled", send_disabled);

	send_button->add_theme_color_override("font_color", AIColors::TEXT_PRIMARY);
	send_button->add_theme_color_override("font_hover_color", AIColors::TEXT_PRIMARY);
	send_button->add_theme_color_override("font_pressed_color", AIColors::TEXT_PRIMARY);
	send_button->add_theme_color_override("font_disabled_color", AIColors::TEXT_DISABLED);

	button_column->add_child(send_button);

	// === Neutral button styles (for Clear) ===
	Ref<StyleBoxFlat> neutral_normal;
	neutral_normal.instantiate();
	neutral_normal->set_bg_color(AIColors::BG_2);
	neutral_normal->set_border_width_all(1);
	neutral_normal->set_border_color(AIColors::BORDER);
	neutral_normal->set_corner_radius_all(AIColors::CORNER_RADIUS_MD * EDSCALE);
	neutral_normal->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);

	Ref<StyleBoxFlat> neutral_hover;
	neutral_hover.instantiate();
	neutral_hover->set_bg_color(AIColors::BG_3);
	neutral_hover->set_border_width_all(1);
	neutral_hover->set_border_color(AIColors::BORDER_LIGHT);
	neutral_hover->set_corner_radius_all(AIColors::CORNER_RADIUS_MD * EDSCALE);
	neutral_hover->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);

	Ref<StyleBoxFlat> neutral_pressed;
	neutral_pressed.instantiate();
	neutral_pressed->set_bg_color(AIColors::BG_1);
	neutral_pressed->set_border_width_all(1);
	neutral_pressed->set_border_color(AIColors::BORDER);
	neutral_pressed->set_corner_radius_all(AIColors::CORNER_RADIUS_MD * EDSCALE);
	neutral_pressed->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);

	Ref<StyleBoxFlat> neutral_disabled;
	neutral_disabled.instantiate();
	neutral_disabled->set_bg_color(AIColors::BG_1);
	neutral_disabled->set_border_width_all(1);
	neutral_disabled->set_border_color(AIColors::BG_2);
	neutral_disabled->set_corner_radius_all(AIColors::CORNER_RADIUS_MD * EDSCALE);
	neutral_disabled->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);

	// === Clear button ===
	clear_button = memnew(Button);
	clear_button->set_text(TTR("Clear"));
	clear_button->connect(SceneStringNames::get_singleton()->pressed, callable_mp(this, &AIStatusPanel::_on_clear_pressed));
	clear_button->add_theme_style_override("normal", neutral_normal);
	clear_button->add_theme_style_override("hover", neutral_hover);
	clear_button->add_theme_style_override("pressed", neutral_pressed);
	clear_button->add_theme_style_override("disabled", neutral_disabled);
	clear_button->add_theme_color_override("font_color", AIColors::TEXT_SECONDARY);
	clear_button->add_theme_color_override("font_hover_color", AIColors::TEXT_PRIMARY);
	clear_button->add_theme_color_override("font_pressed_color", AIColors::TEXT_PRIMARY);
	clear_button->add_theme_color_override("font_disabled_color", AIColors::TEXT_DISABLED);
	button_column->add_child(clear_button);

	// ========================================
	// Status bar (bottom) - compact and subtle
	// ========================================
	HBoxContainer *status_bar = memnew(HBoxContainer);
	status_bar->add_theme_constant_override("separation", 4 * EDSCALE); // Tight spacing between indicator and label
	status_bar->set_v_size_flags(SIZE_SHRINK_CENTER); // Don't expand vertically
	add_child(status_bar);

	// Status indicator (colored circle)
	status_indicator = memnew(AIStatusIndicator);
	status_indicator->set_tooltip_text(TTR("API connection status"));
	status_bar->add_child(status_indicator);

	// Status label - muted appearance
	status_label = memnew(Label);
	status_label->set_text(TTR("Unknown"));
	status_label->add_theme_color_override("font_color", AIColors::TEXT_MUTED);
	status_bar->add_child(status_label);

	// Spacer to push queue label to the right
	Control *status_spacer = memnew(Control);
	status_spacer->set_h_size_flags(SIZE_EXPAND_FILL);
	status_bar->add_child(status_spacer);

	// Queue count label (shown when messages are queued)
	queue_count_label = memnew(Label);
	queue_count_label->set_visible(false);
	queue_count_label->add_theme_color_override("font_color", AIColors::WARNING);
	status_bar->add_child(queue_count_label);

	// ========================================
	// HTTP request nodes for connectivity checks
	// ========================================
	http_openai = memnew(HTTPRequest);
	http_openai->set_timeout(10.0); // 10 second timeout
	add_child(http_openai);
	http_openai->connect("request_completed", callable_mp(this, &AIStatusPanel::_on_openai_request_completed));

	http_gemini = memnew(HTTPRequest);
	http_gemini->set_timeout(10.0);
	add_child(http_gemini);
	http_gemini->connect("request_completed", callable_mp(this, &AIStatusPanel::_on_gemini_request_completed));

	http_xai = memnew(HTTPRequest);
	http_xai->set_timeout(10.0);
	add_child(http_xai);
	http_xai->connect("request_completed", callable_mp(this, &AIStatusPanel::_on_xai_request_completed));
}

AIStatusPanel::~AIStatusPanel() {
	// Disconnect from AI provider signal if connected
	if (Engine::get_singleton()->has_singleton("AI")) {
		Object *ai_obj = Engine::get_singleton()->get_singleton_object("AI");
		AI *ai = Object::cast_to<AI>(ai_obj);
		if (ai) {
			Ref<AIProvider> provider = ai->get_provider();
			if (provider.is_valid()) {
				if (provider->is_connected("request_completed", callable_mp(this, &AIStatusPanel::_on_ai_response))) {
					provider->disconnect("request_completed", callable_mp(this, &AIStatusPanel::_on_ai_response));
				}
			}

			// Disconnect from orchestrator signals if connected
			Ref<AgenticOrchestrator> orchestrator = ai->get_orchestrator();
			if (orchestrator.is_valid()) {
				if (orchestrator->is_connected("progress_update", callable_mp(this, &AIStatusPanel::_on_orchestrator_progress))) {
					orchestrator->disconnect("progress_update", callable_mp(this, &AIStatusPanel::_on_orchestrator_progress));
				}
				if (orchestrator->is_connected("tool_result_ready", callable_mp(this, &AIStatusPanel::_on_orchestrator_tool_result))) {
					orchestrator->disconnect("tool_result_ready", callable_mp(this, &AIStatusPanel::_on_orchestrator_tool_result));
				}
				if (orchestrator->is_connected("run_complete", callable_mp(this, &AIStatusPanel::_on_orchestrator_complete))) {
					orchestrator->disconnect("run_complete", callable_mp(this, &AIStatusPanel::_on_orchestrator_complete));
				}
			}
		}
	}
}

// ============================================================================
// AIStatusIndicatorPlugin - The EditorPlugin that manages everything
// ============================================================================

void AIStatusIndicatorPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			// Create the panel and add it to dock (right side, upper area)
			panel = memnew(AIStatusPanel);
			add_control_to_dock(DOCK_SLOT_RIGHT_UL, panel);

			// Create fallback timer (5 minutes = 300 seconds)
			fallback_timer = memnew(Timer);
			fallback_timer->set_wait_time(300.0);
			fallback_timer->set_one_shot(false);
			fallback_timer->connect("timeout", callable_mp(this, &AIStatusIndicatorPlugin::_on_fallback_timer_timeout));
			add_child(fallback_timer);
			fallback_timer->start();
		} break;

		case NOTIFICATION_EXIT_TREE: {
			if (fallback_timer) {
				fallback_timer->stop();
				remove_child(fallback_timer);
				memdelete(fallback_timer);
				fallback_timer = nullptr;
			}

			if (panel) {
				remove_control_from_docks(panel);
				memdelete(panel);
				panel = nullptr;
			}
		} break;

		case NOTIFICATION_APPLICATION_FOCUS_IN: {
			// Check connectivity when editor regains focus
			if (panel) {
				print_verbose("AI Status: Editor focus regained, checking connectivity...");
				panel->check_api_connectivity();
			}
		} break;
	}
}

void AIStatusIndicatorPlugin::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_on_fallback_timer_timeout"), &AIStatusIndicatorPlugin::_on_fallback_timer_timeout);
}

void AIStatusIndicatorPlugin::_on_fallback_timer_timeout() {
	// Periodic connectivity check (fallback)
	if (panel) {
		print_verbose("AI Status: Periodic connectivity check...");
		panel->check_api_connectivity();
	}
}

AIStatusIndicatorPlugin::AIStatusIndicatorPlugin() {
}

AIStatusIndicatorPlugin::~AIStatusIndicatorPlugin() {
}
