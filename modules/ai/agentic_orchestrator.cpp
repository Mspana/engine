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
#include "core/os/os.h"
#include "core/os/time.h"
#include "scene/main/scene_tree.h"
#include "scene/main/timer.h"

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
	ClassDB::bind_method(D_METHOD("set_user_message_id", "user_message_id"), &AgenticOrchestrator::set_user_message_id);

	// Bind the internal callback so it can be connected via signal
	ClassDB::bind_method(D_METHOD("_on_provider_response", "success", "response", "error"), &AgenticOrchestrator::_on_provider_response);

	// Bind deferred processing method (called on next frame to avoid ProgressDialog issues)
	ClassDB::bind_method(D_METHOD("_process_model_response_deferred"), &AgenticOrchestrator::_process_model_response_deferred);

	// Async run_and_screenshot callbacks
	ClassDB::bind_method(D_METHOD("_run_and_screenshot_tick_gen", "gen"), &AgenticOrchestrator::_run_and_screenshot_tick_gen);
	ClassDB::bind_method(D_METHOD("_on_async_rns_capture_received", "b64"), &AgenticOrchestrator::_on_async_rns_capture_received);

	ADD_SIGNAL(MethodInfo("run_started"));
	ADD_SIGNAL(MethodInfo("api_round_started", PropertyInfo(Variant::INT, "turn")));
	ADD_SIGNAL(MethodInfo("progress_update", PropertyInfo(Variant::STRING, "status"), PropertyInfo(Variant::INT, "turn")));
	ADD_SIGNAL(MethodInfo("tool_result_ready", PropertyInfo(Variant::DICTIONARY, "tool_result")));
	ADD_SIGNAL(MethodInfo("run_complete", PropertyInfo(Variant::BOOL, "success"), PropertyInfo(Variant::STRING, "final_message")));
	ADD_SIGNAL(MethodInfo("checkpoint_recommended", PropertyInfo(Variant::INT, "user_message_id")));
	// assistant_item_ready: canonical assistant item {role:"assistant", content:[...blocks...]}
	// Emitted before any tool calls in the response are executed. Replaces narration_ready/thinking_ready
	// for persistence. narration_ready still emitted for legacy UI consumers.
	ADD_SIGNAL(MethodInfo("assistant_item_ready", PropertyInfo(Variant::DICTIONARY, "item")));
	ADD_SIGNAL(MethodInfo("narration_ready", PropertyInfo(Variant::STRING, "text")));
	ADD_SIGNAL(MethodInfo("todos_updated", PropertyInfo(Variant::ARRAY, "todos")));
	ADD_SIGNAL(MethodInfo("turn_tokens_ready", PropertyInfo(Variant::INT, "tokens")));
	ClassDB::bind_method(D_METHOD("set_todos", "todos"), &AgenticOrchestrator::set_todos);
	ClassDB::bind_method(D_METHOD("inject_user_message", "message"), &AgenticOrchestrator::inject_user_message);
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

	// Always inject game session context (running state + stack trace errors + screenshot)
	{
		AI *ai_singleton = AI::get_singleton();
		if (ai_singleton) {
			Dictionary session_ctx = ai_singleton->consume_session_context();

			if ((bool)session_ctx.get("include", true)) {
				bool game_running = session_ctx.get("game_is_running", false);
				int error_count = session_ctx.get("error_count", 0);

				String ctx_text = "[GAME SESSION]\n";
				ctx_text += vformat("Status: %s\n", game_running ? "Running" : "Not running");
				if (error_count == 0) {
					ctx_text += "Errors: 0 (none)\n";
				} else {
					ctx_text += vformat("Errors: %d\n", error_count);
					if (session_ctx.has("errors")) {
						ctx_text += vformat("%s\n", (String)session_ctx["errors"]);
					}
				}

				Dictionary ctx_msg;
				ctx_msg["role"] = "user";
				ctx_msg["content"] = ctx_text;
				if (session_ctx.has("screenshot_b64")) {
					Array imgs;
					imgs.push_back(session_ctx["screenshot_b64"]);
					ctx_msg["_images"] = imgs;
				}
				// Insert before the last user message (the current prompt)
				current_run.conversation_history.insert(
						MAX(0, current_run.conversation_history.size() - 1), ctx_msg);
			}
		}
	}

	current_run.run_messages.clear();
	current_run.model_turns = 0;
	current_run.total_actions = 0;
	current_run.cancelled = false;
	current_run.user_message = "";
	current_run.user_message_id = 0;
	current_run.todos.clear();
	current_run.has_todos = false;

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

	// Consume any pending user injection before sending to the model
	if (!_pending_user_injection.is_empty()) {
		Dictionary user_msg;
		user_msg["role"] = "user";
		user_msg["content"] = _pending_user_injection;
		current_run.conversation_history.push_back(user_msg);
		print_line(vformat("AgenticOrchestrator: Injected user message: %s", _pending_user_injection.substr(0, 80)));
		_pending_user_injection = "";
	}

	// Increment turn counter
	current_run.model_turns++;
	_waiting_for_response = true;
	emit_signal("api_round_started", current_run.model_turns);

	print_line(vformat("AgenticOrchestrator: Sending model request (turn %d/%d)...", current_run.model_turns, MAX_MODEL_TURNS_PER_RUN));

	// Emit progress update to indicate we're thinking
	_emit_progress_update(vformat("Thinking... (turn %d)", current_run.model_turns), current_run.model_turns);

	// Build message list, injecting current TODO state if present
	Array messages_to_send = current_run.conversation_history;
	if (current_run.has_todos) {
		messages_to_send = current_run.conversation_history.duplicate();
		String block = "[CURRENT_TODOS]\n";
		for (int i = 0; i < current_run.todos.size(); i++) {
			const TodoItem &t = current_run.todos[i];
			String icon = (t.status == "completed") ? "[x]" : (t.status == "in_progress") ? "[-]" : "[ ]";
			block += icon + " " + t.id + ": " + t.content + "\n";
		}
		block += "[/CURRENT_TODOS]\nUpdate this list using update_todos as you complete steps.";
		Dictionary todos_msg;
		todos_msg["role"] = "user";
		todos_msg["content"] = block;
		messages_to_send.push_back(todos_msg);
	}

	// Debug: print full conversation being sent
	print_line("====== AI REQUEST (full conversation) ======");
	for (int i = 0; i < messages_to_send.size(); i++) {
		Dictionary msg = messages_to_send[i];
		String role = msg.get("role", "?");
		String content = msg.get("content", "");
		bool has_images = msg.has("_images") && !msg["_images"].operator Array().is_empty();
		String preview = content.length() > 500 ? content.substr(0, 500) + "... [TRUNCATED]" : content;
		print_line(vformat("  [%d] role=%s images=%s\n%s", i, role, has_images ? "YES" : "no", preview));
	}
	print_line("============================================");

	// Call provider asynchronously - response will come via _on_provider_response
	provider->send_request_with_messages(messages_to_send, "");
}

void AgenticOrchestrator::_on_provider_response(bool p_success, const String &p_response, const String &p_error) {
	_waiting_for_response = false;

	// Debug: print response summary
	print_line("====== AI RESPONSE ======");
	if (p_success) {
		String preview = p_response.length() > 2000 ? p_response.substr(0, 2000) + "... [TRUNCATED]" : p_response;
		print_line(preview);
	} else {
		print_line(vformat("ERROR: %s", p_error));
	}
	print_line("=========================");

	if (!_is_running) {
		print_line("AgenticOrchestrator: Received response but run was already stopped. Ignoring.");
		return;
	}

	if (current_run.cancelled) {
		print_line("AgenticOrchestrator: Received response but cancellation was requested.");
		_handle_cancellation();
		return;
	}

	if (!p_success) {
		ERR_PRINT(vformat("AgenticOrchestrator: Provider request failed: %s", p_error));
		_emit_run_complete(false, vformat("Error: Model request failed - %s", p_error));
		_is_running = false;
		return;
	}

	// Parse the full API response JSON
	JSON json;
	Error err = json.parse(p_response);
	if (err != OK) {
		ERR_PRINT(vformat("AgenticOrchestrator: Failed to parse API response JSON: %s", json.get_error_message()));
		_emit_run_complete(false, "Error: Invalid JSON in API response");
		_is_running = false;
		return;
	}

	Variant parsed_data = json.get_data();
	if (parsed_data.get_type() != Variant::DICTIONARY) {
		ERR_PRINT("AgenticOrchestrator: API response is not a JSON object");
		_emit_run_complete(false, "Error: API response is not a JSON object");
		_is_running = false;
		return;
	}

	// Store and defer processing to next frame (avoids ProgressDialog conflicts)
	_pending_response = parsed_data;
	call_deferred("_process_model_response_deferred");
}

void AgenticOrchestrator::_process_model_response_deferred() {
	if (!_is_running) {
		print_line("AgenticOrchestrator: Deferred processing but run was already stopped. Ignoring.");
		return;
	}

	if (current_run.cancelled) {
		print_line("AgenticOrchestrator: Deferred processing but cancellation was requested.");
		_handle_cancellation();
		return;
	}

	_process_native_tool_response(_pending_response);
	_pending_response.clear();
}

void AgenticOrchestrator::_process_native_tool_response(const Dictionary &p_api_response) {
	// Extract from OpenAI-compatible response: choices[0].message + choices[0].finish_reason
	if (!p_api_response.has("choices")) {
		ERR_PRINT("AgenticOrchestrator: API response missing 'choices' array");
		_emit_run_complete(false, "Error: Unexpected API response format (no choices)");
		_is_running = false;
		return;
	}

	Array choices = p_api_response["choices"];
	if (choices.is_empty()) {
		ERR_PRINT("AgenticOrchestrator: API response has empty choices array");
		_emit_run_complete(false, "Error: API returned empty choices");
		_is_running = false;
		return;
	}

	Dictionary choice = choices[0];
	String finish_reason = choice.get("finish_reason", "stop");
	Dictionary message = choice.get("message", Dictionary());

	// Extract token usage for display in UI
	_current_turn_tokens = 0;
	if (p_api_response.has("usage")) {
		Dictionary usage = p_api_response["usage"];
		_current_turn_tokens = (int)usage.get("total_tokens", 0);
	}
	if (_current_turn_tokens > 0) {
		emit_signal("turn_tokens_ready", _current_turn_tokens);
	}

	String content = message.get("content", "");
	Array tool_calls = message.get("tool_calls", Array());

	// Store tool_calls for potential synthetic cancel use
	_current_tool_calls = tool_calls;

	print_line(vformat("AgenticOrchestrator: finish_reason=%s, content_len=%d, tool_calls=%d",
			finish_reason, content.length(), tool_calls.size()));

	// --- Build canonical assistant item (content_blocks) -------------------
	// This is emitted via assistant_item_ready BEFORE executing any tools,
	// so the store records the assistant message before its results arrive.
	Array content_blocks;
	if (!content.is_empty()) {
		Dictionary text_block;
		text_block["type"] = "text";
		text_block["text"] = content;
		content_blocks.push_back(text_block);
	}
	for (int i = 0; i < tool_calls.size(); i++) {
		Dictionary tc = tool_calls[i];
		Dictionary function = tc.get("function", Dictionary());
		JSON args_parser;
		Dictionary args;
		if (args_parser.parse(String(function.get("arguments", "{}"))) == OK &&
				args_parser.get_data().get_type() == Variant::DICTIONARY) {
			args = args_parser.get_data();
		}
		Dictionary tc_block;
		tc_block["type"] = "tool_call";
		tc_block["id"] = tc.get("id", "");
		tc_block["name"] = function.get("name", "");
		tc_block["args"] = args;
		content_blocks.push_back(tc_block);
	}
	Dictionary assistant_canonical;
	assistant_canonical["role"] = "assistant";
	assistant_canonical["content"] = content_blocks;
	// Emit canonical item for persistence (store handles it before tools run)
	emit_signal("assistant_item_ready", assistant_canonical);

	// --- Also push in OpenAI wire format for in-memory conversation_history --
	// (used by _send_model_request → provider, not persisted to JSONL)
	Dictionary assistant_wire;
	assistant_wire["role"] = "assistant";
	assistant_wire["content"] = content.is_empty() ? Variant() : Variant(content);
	if (!tool_calls.is_empty()) {
		assistant_wire["tool_calls"] = tool_calls;
	}
	current_run.conversation_history.push_back(assistant_wire);

	// Emit mid-turn narration for legacy UI consumers (text present + tool calls follow)
	if (!content.is_empty() && !tool_calls.is_empty()) {
		emit_signal("narration_ready", content);
	}

	// Check if turn is done (no tool calls)
	if (finish_reason == "stop" || (finish_reason != "tool_calls" && tool_calls.is_empty())) {
		print_line(vformat("AgenticOrchestrator: Final response (finish_reason=%s): %s",
				finish_reason, content.length() > 200 ? content.substr(0, 200) + "..." : content));
		_emit_run_complete(true, content);
		_is_running = false;
		return;
	}

	// finish_reason == "tool_calls" — execute each tool call
	_emit_progress_update(vformat("Executing %d tool(s)...", tool_calls.size()), current_run.model_turns);
	print_line(vformat("AgenticOrchestrator: Executing %d tool call(s)...", tool_calls.size()));

	for (int i = 0; i < tool_calls.size(); i++) {
		if (current_run.cancelled) {
			// Emit synthetic cancelled results for all remaining tool calls (no-orphan invariant)
			for (int j = i; j < tool_calls.size(); j++) {
				Dictionary tc = tool_calls[j];
				String call_id = tc.get("id", "");
				Dictionary function = tc.get("function", Dictionary());
				String tool_name = function.get("name", "");
				JSON args_parser;
				Dictionary args;
				if (args_parser.parse(String(function.get("arguments", "{}"))) == OK &&
						args_parser.get_data().get_type() == Variant::DICTIONARY) {
					args = args_parser.get_data();
				}

				// Add to in-memory history (wire format)
				Dictionary cancelled_wire;
				cancelled_wire["role"] = "tool";
				cancelled_wire["tool_call_id"] = call_id;
				Dictionary cancelled_content_dict;
				cancelled_content_dict["status"] = "cancelled";
				cancelled_content_dict["tool_name"] = tool_name;
				cancelled_content_dict["reason"] = "user_cancelled_run";
				cancelled_wire["content"] = JSON::stringify(cancelled_content_dict);
				current_run.conversation_history.push_back(cancelled_wire);
				current_run.run_messages.push_back(cancelled_wire);

				// Emit as tool result so UI/store records the canonical item
				Dictionary trd;
				trd["tool_name"] = tool_name;
				trd["action_id"] = call_id;
				trd["type"] = tool_name;
				trd["args"] = args;
				trd["status"] = "cancelled";
				trd["tokens"] = 0;
				_emit_tool_result(trd);
			}
			break;
		}

		Dictionary tc = tool_calls[i];
		String call_id = tc.get("id", "");
		Dictionary function = tc.get("function", Dictionary());
		String tool_name = function.get("name", "");
		String arguments_json = function.get("arguments", "{}");

		// Parse the arguments JSON string
		JSON args_parser;
		Dictionary args;
		if (args_parser.parse(arguments_json) == OK && args_parser.get_data().get_type() == Variant::DICTIONARY) {
			args = args_parser.get_data();
		} else {
			WARN_PRINT(vformat("AgenticOrchestrator: Failed to parse tool call arguments for %s: %s", tool_name, arguments_json));
		}

		print_line(vformat("  - Tool call %d/%d: %s (id=%s)", i + 1, tool_calls.size(), tool_name, call_id));

		// Handle run_and_screenshot asynchronously
		if (tool_name == "run_and_screenshot") {
			_async_rns_tool_call_id = call_id;
			_async_rns_action_args = args;
			_async_rns_wait_seconds = (float)args.get("wait_seconds", 2.0f);
			_async_rns_phase = ASYNC_RNS_POLL_START;
			_async_rns_phase_start_ms = Time::get_singleton()->get_ticks_msec();

			// Build action dict for execute_single_action
			Dictionary action;
			action["action"] = tool_name;
			action["args"] = args;

			AI *ai = AI::get_singleton();
			if (ai) {
				Dictionary run_result = ai->execute_single_action(action);
				print_line(vformat("AI_RNS: game launch result status=%s", (String)run_result.get("status", "?")));
			}

			_schedule_rns_tick(0.1f);
			// Async — loop will be resumed by _on_async_rns_complete
			return;
		}

		// Execute the tool call
		Dictionary tool_result_msg = _execute_tool_call(call_id, tool_name, args);
		current_run.conversation_history.push_back(tool_result_msg);
		current_run.run_messages.push_back(tool_result_msg);
		current_run.total_actions++;

		// Emit for UI display
		if (tool_result_msg.has("_tool_result_data")) {
			Dictionary trd = tool_result_msg["_tool_result_data"];
			// Screenshots: base64 inflates content length drastically; image tokens depend on
			// dimensions, not file size. Use a fixed estimate (~1000 tokens for a typical screenshot).
			int estimated_tokens;
			if (String(trd.get("type", "")) == "run_and_screenshot") {
				estimated_tokens = 1000;
			} else {
				String tool_content = tool_result_msg.get("content", String());
				estimated_tokens = tool_content.length() / 4;
			}
			trd["tokens"] = estimated_tokens;
			_emit_tool_result(trd);
		}
	}

	if (current_run.cancelled) {
		_handle_cancellation();
		return;
	}

	// Continue the loop
	_send_model_request();
}

Dictionary AgenticOrchestrator::_execute_tool_call(const String &p_call_id, const String &p_tool_name, const Dictionary &p_args) {
	AI *ai = AI::get_singleton();

	Dictionary exec_result;
	if (!ai) {
		exec_result["status"] = "error";
		Dictionary error_dict;
		error_dict["code"] = "internal_error";
		error_dict["message"] = "AI singleton not available";
		exec_result["error"] = error_dict;
	} else {
		// Build action dict compatible with execute_single_action
		Dictionary action;
		action["action"] = p_tool_name;
		action["args"] = p_args;
		exec_result = ai->execute_single_action(action);
	}

	// Build tool result data for UI display
	Dictionary tool_result_data;
	tool_result_data["tool_name"] = p_tool_name;
	tool_result_data["action_id"] = p_call_id;
	tool_result_data["type"] = p_tool_name;
	tool_result_data["args"] = p_args;

	String status = exec_result.get("status", "error");
	tool_result_data["status"] = status;

	if (status == "success") {
		Dictionary result_inner = exec_result.get("result", Dictionary());
		// Use _display_args if provided
		if (result_inner.has("_display_args")) {
			tool_result_data["args"] = result_inner["_display_args"];
			result_inner.erase("_display_args");
		}
		tool_result_data["result"] = result_inner;
	} else {
		tool_result_data["error"] = exec_result.get("error", Dictionary());
	}

	// Build native tool result message (role="tool" with tool_call_id)
	Dictionary message;
	message["role"] = "tool";
	message["tool_call_id"] = p_call_id;
	message["content"] = JSON::stringify(exec_result);

	// Attach structured data for UI (not sent to API, just for display)
	message["_tool_result_data"] = tool_result_data;

	return message;
}

// Old validation/legacy methods removed — native tool-calling handles this via the API

// (Old _validate_response, _is_final_response, _execute_actions, _create_tool_result,
//  _create_validation_error_result, _generate_action_id removed — native tool-calling replaces them)

void AgenticOrchestrator::_handle_cancellation() {
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

void AgenticOrchestrator::set_todos(const Array &p_todos) {
	current_run.todos.clear();
	current_run.has_todos = false;

	for (int i = 0; i < p_todos.size(); i++) {
		if (p_todos[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		Dictionary item = p_todos[i];
		TodoItem todo;
		todo.id = item.get("id", "");
		todo.content = item.get("content", "");
		todo.status = item.get("status", "pending");
		current_run.todos.push_back(todo);
		current_run.has_todos = true;
	}

	emit_signal("todos_updated", p_todos);
}

void AgenticOrchestrator::_emit_progress_update(const String &p_status, int p_turn) {
	emit_signal("progress_update", p_status, p_turn);
}

void AgenticOrchestrator::_emit_tool_result(const Dictionary &p_tool_result) {
	emit_signal("tool_result_ready", p_tool_result);
}

void AgenticOrchestrator::_emit_run_complete(bool p_success, const String &p_final_message) {
	_pending_user_injection = "";
	emit_signal("run_complete", p_success, p_final_message);

	// On successful run completion, emit checkpoint_recommended signal
	// so UI can create a checkpoint anchored to the user message that started this run
	if (p_success && current_run.user_message_id != 0) {
		emit_signal("checkpoint_recommended", current_run.user_message_id);
	}
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

bool AgenticOrchestrator::inject_user_message(const String &p_message) {
	if (!_is_running || current_run.cancelled) {
		return false;
	}
	if (_pending_user_injection.is_empty()) {
		_pending_user_injection = p_message;
	} else {
		_pending_user_injection += "\n\n" + p_message;
	}
	return true;
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

int AgenticOrchestrator::get_repair_cycles() const {
	return 0; // No longer used — kept for API compat
}

String AgenticOrchestrator::get_user_message() const {
	return current_run.user_message;
}

int64_t AgenticOrchestrator::get_user_message_id() const {
	return current_run.user_message_id;
}

void AgenticOrchestrator::set_user_message_id(int64_t p_user_message_id) {
	current_run.user_message_id = p_user_message_id;
}

Array AgenticOrchestrator::get_conversation_history() const {
	return current_run.conversation_history;
}

// ---------------------------------------------------------------------------
// Async run_and_screenshot state machine
// ---------------------------------------------------------------------------

void AgenticOrchestrator::_schedule_rns_tick(float p_delay) {
	_rns_tick_gen++;
	SceneTree *tree = Object::cast_to<SceneTree>(OS::get_singleton()->get_main_loop());
	ERR_FAIL_NULL(tree);
	Ref<SceneTreeTimer> timer = tree->create_timer(p_delay);
	timer->connect("timeout", callable_mp(this, &AgenticOrchestrator::_run_and_screenshot_tick_gen).bind(_rns_tick_gen), CONNECT_ONE_SHOT);
}

void AgenticOrchestrator::_run_and_screenshot_tick_gen(uint32_t p_gen) {
	if (p_gen != _rns_tick_gen) {
		return; // Stale timer from a prior schedule — discard silently
	}
	_run_and_screenshot_tick();
}

void AgenticOrchestrator::_run_and_screenshot_tick() {
	AI *ai = AI::get_singleton();
	uint64_t now_ms = Time::get_singleton()->get_ticks_msec();

	if (_async_rns_phase == ASYNC_RNS_POLL_START) {
		// Wait until the game reports it is running (up to 8s).
		bool game_running = ai && (bool)ai->call("get_game_is_running");
		print_line(vformat("AI_RNS: POLL_START game_running=%s elapsed=%dms",
				game_running ? "YES" : "NO", (int)(now_ms - _async_rns_phase_start_ms)));
		if (game_running) {
			_async_rns_phase = ASYNC_RNS_WAIT_VISUAL;
			_async_rns_phase_start_ms = now_ms;
			_schedule_rns_tick(0.1f);
		} else if (now_ms - _async_rns_phase_start_ms > 8000) {
			_async_rns_phase = ASYNC_RNS_INACTIVE;
			Dictionary err;
			err["status"] = "error";
			Dictionary ed; ed["code"] = "operation_failed"; ed["message"] = "Game did not start within 8 seconds."; ed["details"] = Dictionary();
			err["error"] = ed;
			_on_async_rns_complete(err);
		} else {
			_schedule_rns_tick(0.15f);
		}

	} else if (_async_rns_phase == ASYNC_RNS_WAIT_VISUAL) {
		float elapsed = (now_ms - _async_rns_phase_start_ms) / 1000.0f;
		print_line(vformat("AI_RNS: WAIT_VISUAL elapsed=%.2fs / %.2fs", elapsed, _async_rns_wait_seconds));
		if (elapsed >= _async_rns_wait_seconds) {
			// Time to capture. Connect one-shot listener then trigger.
			_async_rns_phase = ASYNC_RNS_AWAIT_CAPTURE;
			_async_rns_phase_start_ms = now_ms;

			Callable cb = callable_mp(this, &AgenticOrchestrator::_on_async_rns_capture_received);
			ai->connect("game_screenshot_ready", cb, CONNECT_ONE_SHOT);
			print_line("AI_RNS: triggering game screenshot");
			ai->trigger_game_screenshot();

			// Schedule a timeout tick (10s).
			_schedule_rns_tick(10.0f);
		} else {
			_schedule_rns_tick(0.1f);
		}

	} else if (_async_rns_phase == ASYNC_RNS_AWAIT_CAPTURE) {
		// Real timeout — screenshot never arrived (generation counter ensures this tick is the scheduled 10s one).
		print_line("AI_RNS: AWAIT_CAPTURE timed out");
		Callable cb = callable_mp(this, &AgenticOrchestrator::_on_async_rns_capture_received);
		if (ai && ai->is_connected("game_screenshot_ready", cb)) {
			ai->disconnect("game_screenshot_ready", cb);
		}
		_async_rns_phase = ASYNC_RNS_INACTIVE;
		Dictionary err;
		err["status"] = "error";
		Dictionary ed; ed["code"] = "operation_failed"; ed["message"] = "Screenshot capture timed out (10s)."; ed["details"] = Dictionary();
		err["error"] = ed;
		_on_async_rns_complete(err);
	}
}

void AgenticOrchestrator::_on_async_rns_capture_received(const String &p_b64) {
	print_line(vformat("AI_RNS: capture received, b64 len=%d", p_b64.length()));
	_async_rns_phase = ASYNC_RNS_INACTIVE;

	// Stop the game.
	AI *ai = AI::get_singleton();
	if (ai) {
		ai->call("stop_game");
	}

	Dictionary exec_result;
	if (p_b64.is_empty()) {
		exec_result["status"] = "error";
		Dictionary ed; ed["code"] = "operation_failed"; ed["message"] = "Screenshot capture failed: empty image."; ed["details"] = Dictionary();
		exec_result["error"] = ed;
	} else {
		exec_result["status"] = "success";
		Dictionary rd; rd["screenshot_b64"] = p_b64; rd["format"] = "png";
		exec_result["result"] = rd;
	}
	_on_async_rns_complete(exec_result);
}

void AgenticOrchestrator::_on_async_rns_complete(const Dictionary &p_exec_result) {
	// Build tool result directly (don't re-execute — we already have the result from async capture)
	Dictionary tool_result_data;
	tool_result_data["tool_name"] = "run_and_screenshot";
	tool_result_data["action_id"] = _async_rns_tool_call_id;
	tool_result_data["type"] = "run_and_screenshot";
	tool_result_data["args"] = _async_rns_action_args;

	String status = p_exec_result.get("status", "error");
	tool_result_data["status"] = status;
	if (status == "success") {
		tool_result_data["result"] = p_exec_result.get("result", Dictionary());
	} else {
		tool_result_data["error"] = p_exec_result.get("error", Dictionary());
	}

	// Build native tool result message.
	// Strip screenshot_b64 from the JSON content (it inflates the token count as text)
	// and attach it as _images instead so providers can send it as a real image.
	Dictionary content_for_wire = p_exec_result;
	if (status == "success") {
		Dictionary result = p_exec_result.get("result", Dictionary());
		if (result.has("screenshot_b64")) {
			content_for_wire = p_exec_result.duplicate();
			Dictionary r = result.duplicate();
			r.erase("screenshot_b64");
			r["screenshot"] = "<see attached image>";
			content_for_wire["result"] = r;
		}
	}

	Dictionary message;
	message["role"] = "tool";
	message["tool_call_id"] = _async_rns_tool_call_id;
	message["content"] = JSON::stringify(content_for_wire);
	message["_tool_result_data"] = tool_result_data;

	// Attach screenshot as _images for vision-capable models
	if (status == "success") {
		Dictionary result = p_exec_result.get("result", Dictionary());
		if (result.has("screenshot_b64")) {
			Array imgs;
			imgs.push_back(result["screenshot_b64"]);
			message["_images"] = imgs;
		}
	}

	current_run.conversation_history.push_back(message);
	current_run.run_messages.push_back(message);
	current_run.total_actions++;

	// run_and_screenshot: base64 inflates content length; use fixed image token estimate instead.
	tool_result_data["tokens"] = 1000;
	_emit_tool_result(tool_result_data);

	if (current_run.cancelled) {
		_handle_cancellation();
		return;
	}

	_send_model_request();
}
