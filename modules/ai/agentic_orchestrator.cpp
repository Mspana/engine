/**************************************************************************/
/*  agentic_orchestrator.cpp                                              */
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

#include "agentic_orchestrator.h"
#include "ai.h"
#include "ai_provider.h"

#include "core/io/json.h"
#include "core/os/time.h"

AgenticOrchestrator::AgenticOrchestrator() {
	_is_running = false;
	_waiting_for_response = false;
}

AgenticOrchestrator::~AgenticOrchestrator() {
	// Disconnect from provider if connected
	if (provider.is_valid()) {
		if (provider->is_connected("request_completed", callable_mp(this, &AgenticOrchestrator::_on_provider_response))) {
			provider->disconnect("request_completed", callable_mp(this, &AgenticOrchestrator::_on_provider_response));
		}
	}
}

void AgenticOrchestrator::_bind_methods() {
	ClassDB::bind_method(D_METHOD("run_agentic_loop", "initial_messages", "provider"), &AgenticOrchestrator::run_agentic_loop);
	ClassDB::bind_method(D_METHOD("cancel_run"), &AgenticOrchestrator::cancel_run);
	ClassDB::bind_method(D_METHOD("is_cancelled"), &AgenticOrchestrator::is_cancelled);
	ClassDB::bind_method(D_METHOD("is_running"), &AgenticOrchestrator::is_running);
	ClassDB::bind_method(D_METHOD("get_model_turns"), &AgenticOrchestrator::get_model_turns);
	ClassDB::bind_method(D_METHOD("get_total_actions"), &AgenticOrchestrator::get_total_actions);

	// Bind the internal callback so it can be connected via signal
	ClassDB::bind_method(D_METHOD("_on_provider_response", "success", "response", "error"), &AgenticOrchestrator::_on_provider_response);

	// Bind deferred processing method (called on next frame to avoid ProgressDialog issues)
	ClassDB::bind_method(D_METHOD("_process_model_response_deferred"), &AgenticOrchestrator::_process_model_response_deferred);

	ADD_SIGNAL(MethodInfo("run_started"));
	ADD_SIGNAL(MethodInfo("progress_update", PropertyInfo(Variant::STRING, "status"), PropertyInfo(Variant::INT, "turn")));
	ADD_SIGNAL(MethodInfo("tool_result_ready", PropertyInfo(Variant::DICTIONARY, "tool_result")));
	ADD_SIGNAL(MethodInfo("run_complete", PropertyInfo(Variant::BOOL, "success"), PropertyInfo(Variant::STRING, "final_message")));
}

void AgenticOrchestrator::run_agentic_loop(const Array &p_initial_messages, Ref<AIProvider> p_provider) {
	if (_is_running) {
		ERR_PRINT("AgenticOrchestrator::run_agentic_loop - Already running. Ignoring request.");
		return;
	}

	if (p_provider.is_null()) {
		ERR_PRINT("AgenticOrchestrator::run_agentic_loop - Provider is null. Aborting.");
		_emit_run_complete(false, "Error: No AI provider configured.");
		return;
	}

	// Initialize run context
	current_run.conversation_history = p_initial_messages.duplicate();
	current_run.run_messages.clear();
	current_run.model_turns = 0;
	current_run.total_actions = 0;
	current_run.repair_cycles = 0;
	current_run.cancelled = false;
	current_run.user_message = "";

	// Disconnect from old provider if any
	if (provider.is_valid() && provider != p_provider) {
		if (provider->is_connected("request_completed", callable_mp(this, &AgenticOrchestrator::_on_provider_response))) {
			provider->disconnect("request_completed", callable_mp(this, &AgenticOrchestrator::_on_provider_response));
		}
	}

	provider = p_provider;
	_is_running = true;
	_waiting_for_response = false;

	// Emit run_started signal
	emit_signal("run_started");

	// Connect to provider signal (if not already connected)
	if (!provider->is_connected("request_completed", callable_mp(this, &AgenticOrchestrator::_on_provider_response))) {
		provider->connect("request_completed", callable_mp(this, &AgenticOrchestrator::_on_provider_response));
	}

	print_line("AgenticOrchestrator: Starting agentic loop (async)...");

	// Kick off the first model request
	_send_model_request();
}

void AgenticOrchestrator::_send_model_request() {
	// Check cancellation first
	if (current_run.cancelled) {
		print_line("AgenticOrchestrator: Cancellation requested before sending request.");
		_handle_cancellation();
		return;
	}

	// Check guardrails before each request
	if (current_run.model_turns >= MAX_MODEL_TURNS_PER_RUN) {
		print_line(vformat("AgenticOrchestrator: Max turns (%d) exceeded.", MAX_MODEL_TURNS_PER_RUN));
		_handle_max_turns_exceeded();
		return;
	}
	if (current_run.total_actions >= MAX_ACTIONS_PER_RUN) {
		print_line(vformat("AgenticOrchestrator: Max actions (%d) exceeded.", MAX_ACTIONS_PER_RUN));
		_handle_max_actions_exceeded();
		return;
	}

	// Increment turn counter
	current_run.model_turns++;
	_waiting_for_response = true;

	print_line(vformat("AgenticOrchestrator: Sending model request (turn %d/%d)...", current_run.model_turns, MAX_MODEL_TURNS_PER_RUN));

	// Emit progress update to indicate we're thinking
	_emit_progress_update(vformat("Thinking... (turn %d)", current_run.model_turns), current_run.model_turns);

	// Call provider asynchronously - response will come via _on_provider_response
	provider->send_request_with_messages(current_run.conversation_history, "");
}

void AgenticOrchestrator::_on_provider_response(bool p_success, const String &p_response, const String &p_error) {
	_waiting_for_response = false;

	// Check if we're still supposed to be running (might have been cancelled)
	if (!_is_running) {
		print_line("AgenticOrchestrator: Received response but run was already stopped. Ignoring.");
		return;
	}

	// Check cancellation
	if (current_run.cancelled) {
		print_line("AgenticOrchestrator: Received response but cancellation was requested.");
		_handle_cancellation();
		return;
	}

	// Handle request failure
	if (!p_success) {
		ERR_PRINT(vformat("AgenticOrchestrator: Provider request failed: %s", p_error));
		_emit_run_complete(false, vformat("Error: Model request failed - %s", p_error));
		_is_running = false;
		return;
	}

	// Parse the JSON response
	JSON json;
	Error err = json.parse(p_response);
	if (err != OK) {
		// JSON parse error - this is a validation error that can be repaired
		String validation_error = vformat("Invalid JSON in response: %s", json.get_error_message());
		current_run.repair_cycles++;

		print_line(vformat("AgenticOrchestrator: JSON parse error (repair cycle %d/%d): %s",
				current_run.repair_cycles, MAX_REPAIR_CYCLES, validation_error));

		if (current_run.repair_cycles >= MAX_REPAIR_CYCLES) {
			_handle_max_repairs_exceeded(validation_error);
			return;
		}

		// Send error back to model and retry
		Dictionary error_tool_result_msg = _create_validation_error_result(validation_error, p_response);
		current_run.conversation_history.push_back(error_tool_result_msg);
		if (error_tool_result_msg.has("_tool_result_data")) {
			_emit_tool_result(error_tool_result_msg["_tool_result_data"]);
		}

		// Request again
		_send_model_request();
		return;
	}

	Variant parsed_data = json.get_data();
	if (parsed_data.get_type() != Variant::DICTIONARY) {
		String validation_error = "Response is not a JSON object";
		current_run.repair_cycles++;

		if (current_run.repair_cycles >= MAX_REPAIR_CYCLES) {
			_handle_max_repairs_exceeded(validation_error);
			return;
		}

		Dictionary error_tool_result_msg = _create_validation_error_result(validation_error, p_response);
		current_run.conversation_history.push_back(error_tool_result_msg);
		if (error_tool_result_msg.has("_tool_result_data")) {
			_emit_tool_result(error_tool_result_msg["_tool_result_data"]);
		}

		_send_model_request();
		return;
	}

	// Store the parsed response and defer processing to next frame
	// This avoids ProgressDialog conflicts when actions like save_scene trigger dialogs
	_pending_response = parsed_data;
	call_deferred("_process_model_response_deferred");
}

void AgenticOrchestrator::_process_model_response_deferred() {
	// Check if we're still supposed to be running
	if (!_is_running) {
		print_line("AgenticOrchestrator: Deferred processing but run was already stopped. Ignoring.");
		return;
	}

	// Check cancellation
	if (current_run.cancelled) {
		print_line("AgenticOrchestrator: Deferred processing but cancellation was requested.");
		_handle_cancellation();
		return;
	}

	// Process the stored response
	_process_model_response(_pending_response);
	_pending_response.clear();
}

void AgenticOrchestrator::_process_model_response(const Dictionary &p_response) {
	// Validate response structure
	String validation_error;
	if (!_validate_response(p_response, validation_error)) {
		current_run.repair_cycles++;
		print_line(vformat("AgenticOrchestrator: Validation error (repair cycle %d/%d): %s",
				current_run.repair_cycles, MAX_REPAIR_CYCLES, validation_error));

		if (current_run.repair_cycles >= MAX_REPAIR_CYCLES) {
			_handle_max_repairs_exceeded(validation_error);
			return;
		}

		// Append validation error as tool result
		String raw_response = JSON::stringify(p_response);
		Dictionary error_tool_result_msg = _create_validation_error_result(validation_error, raw_response);
		current_run.conversation_history.push_back(error_tool_result_msg);
		if (error_tool_result_msg.has("_tool_result_data")) {
			_emit_tool_result(error_tool_result_msg["_tool_result_data"]);
		}

		// Retry
		_send_model_request();
		return;
	}

	// Reset repair cycles on successful validation
	current_run.repair_cycles = 0;

	// Check if final response (empty actions array)
	if (_is_final_response(p_response)) {
		String final_message = p_response.get("assistant_text", "");
		print_line(vformat("AgenticOrchestrator: Final response received: %s", final_message));
		_emit_run_complete(true, final_message);
		_is_running = false;
		return;
	}

	// Extract actions and assistant_text
	Array actions = p_response.get("actions", Array());
	String assistant_text = p_response.get("assistant_text", "");

	// Display assistant_text as progress update
	if (!assistant_text.is_empty()) {
		_emit_progress_update(assistant_text, current_run.model_turns);
	}

	// Execute actions and capture results
	if (actions.size() > 0) {
		print_line(vformat("AgenticOrchestrator: Executing %d action(s)...", actions.size()));
		Array tool_results = _execute_actions(actions);
		current_run.total_actions += actions.size();

		// Check for cancellation during action execution
		if (current_run.cancelled) {
			_handle_cancellation();
			return;
		}

		// Append tool results to conversation history
		for (int i = 0; i < tool_results.size(); i++) {
			Dictionary tool_result_msg = tool_results[i];
			current_run.conversation_history.push_back(tool_result_msg);
			current_run.run_messages.push_back(tool_result_msg);

			// Extract the structured data for UI display
			if (tool_result_msg.has("_tool_result_data")) {
				Dictionary tool_result_data = tool_result_msg["_tool_result_data"];
				_emit_tool_result(tool_result_data);
			}
		}

		// Continue the loop by sending next request
		_send_model_request();
	} else {
		WARN_PRINT("AgenticOrchestrator: Received ACTION MODE response with no actions. Treating as validation error.");
		String error_msg = "Response is in ACTION MODE but contains no actions. Either provide actions or use FINAL MODE with an empty 'actions' array and a non-empty 'assistant_text'.";
		Dictionary error_tool_result_msg = _create_validation_error_result(error_msg, JSON::stringify(p_response));
		current_run.conversation_history.push_back(error_tool_result_msg);

		// Extract the structured data for UI display
		if (error_tool_result_msg.has("_tool_result_data")) {
			Dictionary tool_result_data = error_tool_result_msg["_tool_result_data"];
			_emit_tool_result(tool_result_data);
		}

		// Continue loop
		_send_model_request();
	}
}

bool AgenticOrchestrator::_validate_response(const Dictionary &p_response, String &r_error) {
	// Check for required field: "actions"
	if (!p_response.has("actions")) {
		r_error = "Response missing required field: 'actions'. Your response MUST contain an 'actions' array (empty for FINAL MODE, non-empty for ACTION MODE).";
		return false;
	}

	// Check actions is an Array
	if (p_response["actions"].get_type() != Variant::ARRAY) {
		r_error = "Field 'actions' must be an Array";
		return false;
	}

	Array actions = p_response["actions"];

	// Check for assistant_text field (optional but recommended)
	if (!p_response.has("assistant_text")) {
		// This is optional, just warn
		print_verbose("AgenticOrchestrator: Response missing optional field 'assistant_text'");
	}

	// If actions is empty, this should be FINAL MODE - require assistant_text
	if (actions.size() == 0) {
		if (!p_response.has("assistant_text") || p_response["assistant_text"].get_type() != Variant::STRING) {
			r_error = "FINAL MODE requires 'assistant_text' field with non-empty string. You provided an empty 'actions' array which signals FINAL MODE, but 'assistant_text' is missing or invalid.";
			return false;
		}
		String text = p_response["assistant_text"];
		if (text.strip_edges().is_empty()) {
			r_error = "FINAL MODE requires non-empty 'assistant_text'. Your 'actions' array is empty (FINAL MODE) but 'assistant_text' is empty.";
			return false;
		}
	}

	// Validate each action in the array
	for (int i = 0; i < actions.size(); i++) {
		if (actions[i].get_type() != Variant::DICTIONARY) {
			r_error = vformat("Action at index %d is not a Dictionary", i);
			return false;
		}

		Dictionary action = actions[i];
		if (!action.has("action")) {
			r_error = vformat("Action at index %d missing 'action' field", i);
			return false;
		}
		if (!action.has("args")) {
			r_error = vformat("Action at index %d missing 'args' field", i);
			return false;
		}
		if (action["args"].get_type() != Variant::DICTIONARY) {
			r_error = vformat("Action at index %d 'args' field must be a Dictionary", i);
			return false;
		}
	}

	return true;
}

bool AgenticOrchestrator::_is_final_response(const Dictionary &p_response) {
	if (!p_response.has("actions")) {
		return false;
	}

	Array actions = p_response["actions"];
	if (actions.size() == 0) {
		// Empty actions array = FINAL MODE
		// We already validated that assistant_text exists and is non-empty
		return true;
	}

	return false;
}

Array AgenticOrchestrator::_execute_actions(const Array &p_actions) {
	Array tool_results;

	// Get the AI singleton to execute actions
	AI *ai = Object::cast_to<AI>(Engine::get_singleton()->get_singleton_object("AI"));
	if (!ai) {
		ERR_PRINT("AgenticOrchestrator::_execute_actions - AI singleton not found.");
		// Return error tool results for all actions
		for (int i = 0; i < p_actions.size(); i++) {
			Dictionary action = p_actions[i];
			String action_type = action.get("action", "unknown");
			Dictionary action_args = action.get("args", Dictionary());

			Dictionary exec_result;
			exec_result["status"] = "error";
			Dictionary error_dict;
			error_dict["code"] = "internal_error";
			error_dict["message"] = "AI singleton not available";
			error_dict["details"] = Dictionary();
			exec_result["error"] = error_dict;

			Dictionary tool_result = _create_tool_result(action_type, action_args, exec_result);
			tool_results.push_back(tool_result);
		}
		return tool_results;
	}

	// Execute each action sequentially
	for (int i = 0; i < p_actions.size(); i++) {
		// Check for cancellation between actions
		if (current_run.cancelled) {
			print_line("AgenticOrchestrator: Cancelled during action execution.");
			break;
		}

		Dictionary action = p_actions[i];
		String action_type = action.get("action", "unknown");
		Dictionary action_args = action.get("args", Dictionary());

		print_line(vformat("  - Executing action %d/%d: %s", i + 1, p_actions.size(), action_type));

		// Call AI singleton's action executor
		Dictionary exec_result = ai->execute_single_action(action);

		// Create tool result from execution result
		Dictionary tool_result = _create_tool_result(action_type, action_args, exec_result);
		tool_results.push_back(tool_result);
	}

	return tool_results;
}

Dictionary AgenticOrchestrator::_create_tool_result(const String &p_action_type, const Dictionary &p_action_args, const Dictionary &p_exec_result) {
	// Format tool result as a "user" message that the API can understand
	// The content is JSON describing what happened
	Dictionary tool_result_data;
	tool_result_data["tool_name"] = "godot_action_executor";
	tool_result_data["action_id"] = _generate_action_id();
	tool_result_data["type"] = p_action_type;
	tool_result_data["args"] = p_action_args;

	// Extract status and result/error from exec_result
	String status = p_exec_result.get("status", "error");
	tool_result_data["status"] = status;

	if (status == "success") {
		tool_result_data["result"] = p_exec_result.get("result", Dictionary());
	} else {
		tool_result_data["error"] = p_exec_result.get("error", Dictionary());
	}

	// Create API-compatible message with role="user" containing tool result
	Dictionary message;
	message["role"] = "user";
	message["content"] = vformat("[TOOL_RESULT]\n%s", JSON::stringify(tool_result_data, "  "));

	// Also store the structured data for UI display (separate from API message)
	message["_tool_result_data"] = tool_result_data;

	return message;
}

Dictionary AgenticOrchestrator::_create_validation_error_result(const String &p_error_message, const String &p_raw_response) {
	// Format validation error as a "user" message that the API can understand
	Dictionary tool_result_data;
	tool_result_data["tool_name"] = "godot_action_executor";
	tool_result_data["action_id"] = _generate_action_id();
	tool_result_data["type"] = "validation_error";
	tool_result_data["args"] = Variant(); // null
	tool_result_data["status"] = "error";

	Dictionary error_dict;
	error_dict["code"] = "validation_error";
	error_dict["message"] = p_error_message;

	Dictionary details;
	// Truncate raw_response to avoid huge messages
	String truncated_response = p_raw_response;
	if (truncated_response.length() > 500) {
		truncated_response = truncated_response.substr(0, 500) + "... (truncated)";
	}
	details["raw_response_preview"] = truncated_response;
	details["hint"] = "Please output valid JSON with 'assistant_text' (string) and 'actions' (array) fields. For FINAL MODE, use empty actions array. JSON does NOT support comments (// or /* */).";
	error_dict["details"] = details;

	tool_result_data["error"] = error_dict;

	// Create API-compatible message with role="user" containing the error
	Dictionary message;
	message["role"] = "user";
	message["content"] = vformat("[TOOL_RESULT - VALIDATION ERROR]\n%s\n\nPlease fix your response and try again.", JSON::stringify(tool_result_data, "  "));

	// Also store the structured data for UI display
	message["_tool_result_data"] = tool_result_data;

	return message;
}

String AgenticOrchestrator::_generate_action_id() {
	// Generate a simple timestamp-based ID
	// Format: timestamp_ms + random component
	uint64_t time_ms = Time::get_singleton()->get_ticks_msec();
	uint32_t random_component = Math::rand();

	return vformat("%d_%x", time_ms, random_component);
}

void AgenticOrchestrator::_handle_cancellation() {
	// Create cancelled tool result
	Dictionary cancel_result;
	cancel_result["role"] = "tool";
	cancel_result["tool_name"] = "godot_action_executor";
	cancel_result["action_id"] = _generate_action_id();
	cancel_result["type"] = "cancellation";
	cancel_result["args"] = Variant();
	cancel_result["status"] = "cancelled";

	Dictionary error_dict;
	error_dict["code"] = "user_cancelled";
	error_dict["message"] = "User cancelled the operation";
	error_dict["details"] = Dictionary();
	cancel_result["error"] = error_dict;

	_emit_tool_result(cancel_result);
	_emit_run_complete(false, "Cancelled. Tell me what to do next.");
	_is_running = false;
	_waiting_for_response = false;
}

void AgenticOrchestrator::_handle_max_turns_exceeded() {
	String message = vformat("Reached maximum model turns (%d). I was working on your request but hit the turn limit. What would you like me to do next?", MAX_MODEL_TURNS_PER_RUN);
	_emit_run_complete(false, message);
	_is_running = false;
	_waiting_for_response = false;
}

void AgenticOrchestrator::_handle_max_actions_exceeded() {
	String message = vformat("Reached maximum actions (%d). I've executed %d actions so far. What should I do next?", MAX_ACTIONS_PER_RUN, current_run.total_actions);
	_emit_run_complete(false, message);
	_is_running = false;
	_waiting_for_response = false;
}

void AgenticOrchestrator::_handle_max_repairs_exceeded(const String &p_validation_error) {
	String message = vformat("Validation error persists after %d repair attempts: %s. Please rephrase your request or try a different approach.", MAX_REPAIR_CYCLES, p_validation_error);
	_emit_run_complete(false, message);
	_is_running = false;
	_waiting_for_response = false;
}

void AgenticOrchestrator::_emit_progress_update(const String &p_status, int p_turn) {
	emit_signal("progress_update", p_status, p_turn);
}

void AgenticOrchestrator::_emit_tool_result(const Dictionary &p_tool_result) {
	emit_signal("tool_result_ready", p_tool_result);
}

void AgenticOrchestrator::_emit_run_complete(bool p_success, const String &p_final_message) {
	emit_signal("run_complete", p_success, p_final_message);
}

String AgenticOrchestrator::_format_tool_result_for_display(const Dictionary &p_tool_result) {
	String status = p_tool_result.get("status", "unknown");
	String type = p_tool_result.get("type", "unknown");
	Dictionary args = p_tool_result.get("args", Dictionary());

	String result_str;

	if (status == "success") {
		result_str = vformat("[Result] %s succeeded", type);
		if (p_tool_result.has("result")) {
			Dictionary result_data = p_tool_result["result"];
			if (!result_data.is_empty()) {
				result_str += vformat(": %s", JSON::stringify(result_data));
			}
		}
	} else if (status == "error") {
		result_str = vformat("[Result] %s failed", type);
		if (p_tool_result.has("error")) {
			Dictionary error_dict = p_tool_result["error"];
			String error_msg = error_dict.get("message", "Unknown error");
			result_str += vformat(": %s", error_msg);
		}
	} else if (status == "cancelled") {
		result_str = "[Result] Cancelled by user";
	} else {
		result_str = vformat("[Result] %s: unknown status", type);
	}

	return result_str;
}

void AgenticOrchestrator::cancel_run() {
	// Guard: no-op if not running or already cancelled
	if (!_is_running || current_run.cancelled) {
		print_verbose("AgenticOrchestrator::cancel_run - Already cancelled or not running. Ignoring.");
		return;
	}

	current_run.cancelled = true;
	print_line("AgenticOrchestrator: Cancellation requested.");

	// If we're waiting for a response, the cancellation will be handled
	// when the response arrives in _on_provider_response
	// If we're not waiting, the next checkpoint will detect it
}

bool AgenticOrchestrator::is_cancelled() const {
	return current_run.cancelled;
}

bool AgenticOrchestrator::is_running() const {
	return _is_running;
}

int AgenticOrchestrator::get_model_turns() const {
	return current_run.model_turns;
}

int AgenticOrchestrator::get_total_actions() const {
	return current_run.total_actions;
}
