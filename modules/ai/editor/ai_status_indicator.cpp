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
#include "core/io/json.h"
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
// AIStatusIndicator - The colored circle showing connection status
// ============================================================================

void AIStatusIndicator::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_DRAW: {
			// Draw a filled circle
			Color indicator_color;
			switch (current_status) {
				case STATUS_UNKNOWN:
					indicator_color = Color(0.5, 0.5, 0.5); // Gray
					break;
				case STATUS_CHECKING:
					indicator_color = Color(1.0, 0.8, 0.0); // Yellow/Orange
					break;
				case STATUS_CONNECTED:
					indicator_color = Color(0.2, 0.8, 0.2); // Green
					break;
				case STATUS_DISCONNECTED:
					indicator_color = Color(0.8, 0.2, 0.2); // Red
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
	ClassDB::bind_method(D_METHOD("_on_send_pressed"), &AIStatusPanel::_on_send_pressed);
	ClassDB::bind_method(D_METHOD("_on_cancel_pressed"), &AIStatusPanel::_on_cancel_pressed);
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
			Control *bubble = _create_message_bubble(messages[i]);
			if (bubble) {
				message_list->add_child(bubble);
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
	style->set_corner_radius_all(8 * EDSCALE);
	style->set_content_margin_all(10 * EDSCALE);

	bool is_user = p_message.role == "user";

	if (is_user) {
		// User messages: blue tint, right aligned
		style->set_bg_color(Color(0.2, 0.4, 0.6, 0.8));
		// Add flexible spacer on left to push bubble right
		Control *spacer = memnew(Control);
		spacer->set_h_size_flags(SIZE_EXPAND_FILL);
		spacer->set_stretch_ratio(0.3); // Take up to 30% of space
		align_container->add_child(spacer);
		bubble->set_h_size_flags(SIZE_EXPAND_FILL);
		bubble->set_stretch_ratio(0.7); // Bubble takes up to 70%
		align_container->add_child(bubble);
	} else {
		// Assistant messages: gray tint, left aligned
		style->set_bg_color(Color(0.3, 0.3, 0.35, 0.8));
		bubble->set_h_size_flags(SIZE_EXPAND_FILL);
		bubble->set_stretch_ratio(0.9); // Bubble takes up to 90%
		align_container->add_child(bubble);
		// Add flexible spacer on right
		Control *spacer = memnew(Control);
		spacer->set_h_size_flags(SIZE_EXPAND_FILL);
		spacer->set_stretch_ratio(0.1);
		align_container->add_child(spacer);
	}

	bubble->add_theme_style_override("panel", style);

	// Create label for content
	RichTextLabel *label = memnew(RichTextLabel);
	label->set_use_bbcode(true);
	label->set_fit_content(true);
	label->set_scroll_active(false);
	label->set_selection_enabled(true);

	// Display content (no longer need to pretty-print JSON since we extract the message)
	label->add_text(p_message.content);

	bubble->add_child(label);

	return align_container;
}

Control *AIStatusPanel::_create_tool_result_ui(const Dictionary &p_tool_result) {
	// Create container for alignment (tool results are always left-aligned)
	HBoxContainer *align_container = memnew(HBoxContainer);
	align_container->set_h_size_flags(SIZE_EXPAND_FILL);

	// Create panel for the tool result bubble
	PanelContainer *bubble = memnew(PanelContainer);
	bubble->set_h_size_flags(SIZE_EXPAND_FILL);
	align_container->add_child(bubble);

	// Add spacer for right alignment
	align_container->add_spacer();

	// Style the bubble (using a distinct color for tool results)
	Ref<StyleBoxFlat> style;
	style.instantiate();
	style->set_bg_color(Color(0.25, 0.3, 0.35)); // Darker grayish color for tool results
	style->set_content_margin_all(8 * EDSCALE);
	style->set_corner_radius_all(4 * EDSCALE);
	bubble->add_theme_style_override("panel", style);

	// Create label for content
	RichTextLabel *label = memnew(RichTextLabel);
	label->set_use_bbcode(true);
	label->set_fit_content(true);
	label->set_scroll_active(false);
	label->set_selection_enabled(true);

	// Format tool result for display
	String action_type = p_tool_result.get("type", "unknown");
	String status = p_tool_result.get("status", "unknown");

	String display_text = vformat("[b][Tool][/b] %s\n", action_type);

	if (status == "success") {
		display_text += "[color=green]✓ Success[/color]";
		if (p_tool_result.has("result")) {
			Dictionary result = p_tool_result["result"];
			if (!result.is_empty()) {
				display_text += vformat("\n%s", JSON::stringify(result, "  ", false));
			}
		}
	} else if (status == "error") {
		display_text += "[color=red]✗ Error[/color]";
		if (p_tool_result.has("error")) {
			Dictionary error = p_tool_result["error"];
			String error_msg = error.get("message", "Unknown error");
			display_text += vformat("\n%s", error_msg);
		}
	}

	label->add_text(display_text);
	bubble->add_child(label);

	return align_container;
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

	bool can_send = !is_waiting_for_response && !prompt_edit->get_text().strip_edges().is_empty();
	send_button->set_disabled(!can_send);
}

void AIStatusPanel::_update_cancel_button_state() {
	if (!cancel_button) {
		return;
	}

	// Cancel button is enabled when there's an active agentic run
	bool can_cancel = false;
	if (Engine::get_singleton()->has_singleton("AI")) {
		Object *ai_obj = Engine::get_singleton()->get_singleton_object("AI");
		AI *ai = Object::cast_to<AI>(ai_obj);
		if (ai) {
			Ref<AgenticOrchestrator> orchestrator = ai->get_orchestrator();
			can_cancel = orchestrator.is_valid() && orchestrator->is_running();
		}
	}

	cancel_button->set_disabled(!can_cancel);
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

void AIStatusPanel::_on_send_pressed() {
	if (!prompt_edit || is_waiting_for_response) {
		return;
	}

	String prompt_text = prompt_edit->get_text().strip_edges();
	if (prompt_text.is_empty()) {
		return;
	}

	// Append user message to store first
	if (chat_store.is_valid()) {
		ChatMessage user_msg = chat_store->append_message("user", prompt_text);
		_append_message_ui(user_msg);
	}

	// Clear input
	prompt_edit->set_text("");

	// Show pending message
	_show_pending_message();

	// Update state
	is_waiting_for_response = true;
	_update_send_button_state();
	_update_cancel_button_state();

	// Build full message history for context
	Array messages = _build_model_messages();

	// Send to AI via agentic orchestrator for multi-turn tool use
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
				_update_send_button_state();
				_update_cancel_button_state();
			}
		}
	} else {
		ERR_PRINT("AI Chat Panel: AI singleton not found.");
		_remove_pending_message();
		is_waiting_for_response = false;
		_update_send_button_state();
		_update_cancel_button_state();
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
	_update_send_button_state();
	_update_cancel_button_state();
}

void AIStatusPanel::_on_cancel_pressed() {
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
	_update_send_button_state();
	_update_cancel_button_state();

	// Reset status label
	if (status_label) {
		status_label->set_text(p_success ? TTR("Ready") : TTR("Cancelled"));
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

	message_list = memnew(VBoxContainer);
	message_list->set_h_size_flags(SIZE_EXPAND_FILL);
	message_list->add_theme_constant_override("separation", 8 * EDSCALE);
	transcript_scroll->add_child(message_list);

	// ========================================
	// Separator
	// ========================================
	HSeparator *separator = memnew(HSeparator);
	add_child(separator);

	// ========================================
	// Input bar (bottom)
	// ========================================
	HBoxContainer *input_bar = memnew(HBoxContainer);
	input_bar->add_theme_constant_override("separation", 4 * EDSCALE);
	add_child(input_bar);

	// Prompt text edit
	prompt_edit = memnew(TextEdit);
	prompt_edit->set_placeholder(TTR("Type a message..."));
	prompt_edit->set_h_size_flags(SIZE_EXPAND_FILL);
	prompt_edit->set_custom_minimum_size(Size2(0, 60 * EDSCALE));
	prompt_edit->set_line_wrapping_mode(TextEdit::LINE_WRAPPING_BOUNDARY);
	prompt_edit->connect("text_changed", callable_mp(this, &AIStatusPanel::_on_prompt_text_changed));
	input_bar->add_child(prompt_edit);

	// Button column
	VBoxContainer *button_column = memnew(VBoxContainer);
	button_column->add_theme_constant_override("separation", 4 * EDSCALE);
	input_bar->add_child(button_column);

	// Send button
	send_button = memnew(Button);
	send_button->set_text(TTR("Send"));
	send_button->set_disabled(true); // Disabled until text is entered
	send_button->connect(SceneStringNames::get_singleton()->pressed, callable_mp(this, &AIStatusPanel::_on_send_pressed));
	button_column->add_child(send_button);

	// Cancel button (for stopping agentic runs)
	cancel_button = memnew(Button);
	cancel_button->set_text(TTR("Cancel"));
	cancel_button->set_disabled(true); // Disabled unless there's an active run
	cancel_button->connect(SceneStringNames::get_singleton()->pressed, callable_mp(this, &AIStatusPanel::_on_cancel_pressed));
	button_column->add_child(cancel_button);

	// Clear button
	clear_button = memnew(Button);
	clear_button->set_text(TTR("Clear"));
	clear_button->connect(SceneStringNames::get_singleton()->pressed, callable_mp(this, &AIStatusPanel::_on_clear_pressed));
	button_column->add_child(clear_button);

	// ========================================
	// Status bar (bottom)
	// ========================================
	HBoxContainer *status_bar = memnew(HBoxContainer);
	status_bar->add_theme_constant_override("separation", 4 * EDSCALE);
	add_child(status_bar);

	// Status indicator (colored circle)
	status_indicator = memnew(AIStatusIndicator);
	status_indicator->set_tooltip_text(TTR("API connection status"));
	status_bar->add_child(status_indicator);

	// Status label
	status_label = memnew(Label);
	status_label->set_text(TTR("Unknown"));
	status_bar->add_child(status_label);

	// Add spacer to push status to left
	status_bar->add_spacer();

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
