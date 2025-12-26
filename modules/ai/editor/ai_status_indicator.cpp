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
#include "editor/editor_node.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/label.h"
#include "scene/scene_string_names.h"

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
// AIStatusPanel - The bottom panel containing UI elements
// ============================================================================

void AIStatusPanel::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			// Initial connectivity check
			check_api_connectivity();
		} break;

		case NOTIFICATION_THEME_CHANGED: {
			// Update button icon if needed
		} break;
	}
}

void AIStatusPanel::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_on_request_button_pressed"), &AIStatusPanel::_on_request_button_pressed);
	ClassDB::bind_method(D_METHOD("_on_openai_request_completed", "result", "response_code", "headers", "body"), &AIStatusPanel::_on_openai_request_completed);
	ClassDB::bind_method(D_METHOD("_on_gemini_request_completed", "result", "response_code", "headers", "body"), &AIStatusPanel::_on_gemini_request_completed);
	ClassDB::bind_method(D_METHOD("_on_xai_request_completed", "result", "response_code", "headers", "body"), &AIStatusPanel::_on_xai_request_completed);
	ClassDB::bind_method(D_METHOD("check_api_connectivity"), &AIStatusPanel::check_api_connectivity);
}

void AIStatusPanel::_on_request_button_pressed() {
	if (!prompt_edit) {
		return;
	}

	String prompt_text = prompt_edit->get_text();
	if (prompt_text.is_empty()) {
		return;
	}

	// Get the AI singleton
	if (Engine::get_singleton()->has_singleton("AI")) {
		Object *ai_obj = Engine::get_singleton()->get_singleton_object("AI");
		AI *ai = Object::cast_to<AI>(ai_obj);
		if (ai) {
			print_line(vformat("AI Status Panel: Sending prompt: '%s'", prompt_text));
			ai->request_actions(prompt_text);
		}
	} else {
		ERR_PRINT("AI Status Panel: AI singleton not found.");
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
}

AIStatusPanel::AIStatusPanel() {
	set_name("AI");

	// Prompt text edit (fills remaining space)
	prompt_edit = memnew(TextEdit);
	prompt_edit->set_placeholder(TTR("Enter prompt for AI..."));
	prompt_edit->set_h_size_flags(SIZE_EXPAND_FILL);
	prompt_edit->set_v_size_flags(SIZE_EXPAND_FILL);
	add_child(prompt_edit);

	// Create button bar (horizontal container)
	button_bar = memnew(HBoxContainer);
	add_child(button_bar);

	// Add spacer to push button to the right
	button_bar->add_spacer();

	// Request Actions button with status indicator inside
	request_button = memnew(Button);
	request_button->set_text(TTR("Request Actions"));
	request_button->connect(SceneStringName(pressed), callable_mp(this, &AIStatusPanel::_on_request_button_pressed));

	// Status indicator (colored circle) - added as icon to the button
	status_indicator = memnew(AIStatusIndicator);
	status_indicator->set_tooltip_text(TTR("API connection status"));

	// Create an HBox to hold indicator + button together tightly
	HBoxContainer *button_with_status = memnew(HBoxContainer);
	button_with_status->add_theme_constant_override("separation", 4);
	button_with_status->add_child(status_indicator);
	button_with_status->add_child(request_button);
	button_bar->add_child(button_with_status);

	// Create HTTP request nodes for connectivity checks
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

