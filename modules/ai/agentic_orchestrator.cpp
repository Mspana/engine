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
#include "scene_diff.h"

#include "core/io/json.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/script_editor_debugger.h"
#include "editor/editor_log.h"
#include "editor/editor_node.h"
#include "core/config/project_settings.h"
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
	// scene_diff_ready: {attribution: "ai"|"user", scenes: [{path, diff, too_large, added, removed, missing}]}
	// Emitted whenever scene diffs are injected into model context, so the
	// panel can persist them to the chat store (dashboard visibility).
	ADD_SIGNAL(MethodInfo("scene_diff_ready", PropertyInfo(Variant::DICTIONARY, "diff_info")));
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
						Array errors = session_ctx["errors"];
						if (errors.size() > 0) {
							ctx_text += JSON::stringify(errors, "  ") + "\n";
						}
					}
				}
				if (session_ctx.has("game_output")) {
					ctx_text += "\n[GAME OUTPUT]\n";
					ctx_text += (String)session_ctx["game_output"] + "\n";
				}

				if (session_ctx.has("parse_errors")) {
					Array parse_errors = session_ctx["parse_errors"];
					if (parse_errors.size() > 0) {
						String pe_file = session_ctx.get("parse_error_file", "");
						ctx_text += vformat("\n[PARSE ERRORS] %s\n", pe_file);
						ctx_text += JSON::stringify(parse_errors, "  ") + "\n";
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

	// Scene changes made outside the conversation (user edits in the editor,
	// external tools) since the model last saw each tracked scene. Diffed
	// against the per-scene snapshots and injected before the current prompt.
	{
		// Discard stale batch tracking from a previous (e.g. cancelled) run.
		AISceneDiff::take_batch_scenes();

		Vector<String> tracked = AISceneDiff::get_snapshot_paths();
		String block;
		Array changed_scenes;
		for (const String &scene_path : tracked) {
			Dictionary diff = AISceneDiff::diff_scene_against_snapshot(scene_path);
			bool missing = diff.get("missing", false);
			bool changed = diff.get("changed", false);
			if (!missing && !changed) {
				continue;
			}
			String entry = _format_scene_diff_entry(diff);
			if (entry.is_empty()) {
				continue;
			}
			block += entry + "\n";
			changed_scenes.push_back(diff);
		}
		if (!block.is_empty()) {
			Dictionary diff_msg;
			diff_msg["role"] = "user";
			diff_msg["content"] = "[SCENE CHANGES] The following scene file(s) changed outside this conversation (user edits in the editor, or external changes) since you last saw them:\n\n" + block.strip_edges();
			current_run.conversation_history.insert(
					MAX(0, current_run.conversation_history.size() - 1), diff_msg);

			Dictionary info;
			info["attribution"] = "user";
			info["scenes"] = changed_scenes;
			emit_signal("scene_diff_ready", info);
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
	_request_retry_attempt = 0;

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

	// Send conversation history as-is. Todo state is NOT injected per turn: a
	// fresh trailing block would shift position every turn and invalidate the
	// prompt-cache prefix. The model's own update_todos calls (args + echoed
	// result) already carry the state in append-only history.
	Array messages_to_send = current_run.conversation_history;

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

	// Log raw request for dashboard
	{
		Dictionary req_body = provider->build_request_body_with_messages(messages_to_send, "");
		AI::get_singleton()->log_raw_api("request", current_run.model_turns, req_body);
	}

	// Call provider asynchronously - response will come via _on_provider_response
	provider->send_request_with_messages(messages_to_send, "");
}

// True for pre-response transport failures worth retrying. These are the exact
// messages the providers emit before any HTTP response is received. API-level
// failures ("HTTP error: N", parsed error bodies, "API key is not set") are
// deliberately excluded — a retry would not change the outcome.
bool AgenticOrchestrator::_is_transient_network_error(const String &p_error) {
	return p_error.begins_with("Request failed") ||
			p_error.begins_with("Connection failed") ||
			p_error.begins_with("Failed to connect") ||
			p_error.begins_with("Failed to send request");
}

void AgenticOrchestrator::_schedule_request_retry(float p_delay_seconds) {
	SceneTree *tree = Object::cast_to<SceneTree>(OS::get_singleton()->get_main_loop());
	if (!tree) {
		// No main loop to time against — fail the run rather than hang it silently.
		ERR_PRINT("AgenticOrchestrator: cannot schedule retry (no SceneTree); ending run.");
		_emit_run_complete(false, "Error: Model request failed - network error (retry unavailable)");
		_is_running = false;
		return;
	}
	Ref<SceneTreeTimer> timer = tree->create_timer(p_delay_seconds);
	timer->connect("timeout", callable_mp(this, &AgenticOrchestrator::_retry_model_request), CONNECT_ONE_SHOT);
}

void AgenticOrchestrator::_retry_model_request() {
	if (!_is_running) {
		return; // Run ended while the backoff timer was pending.
	}
	if (current_run.cancelled) {
		_handle_cancellation();
		return;
	}
	// A retry re-sends the same turn. _send_model_request increments model_turns, so
	// pre-decrement to keep retries from consuming the model-turn budget.
	current_run.model_turns--;
	_send_model_request();
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
		// Transient network failures (connection reset, DNS, TLS, timeout — common on
		// flaky wifi) are retried with exponential backoff rather than aborting the
		// run. Real API errors (HTTP 4xx/5xx, parsed error bodies, missing key) fall
		// through and end the run — retrying those would not help.
		if (_is_transient_network_error(p_error) && _request_retry_attempt < MAX_REQUEST_RETRIES) {
			_request_retry_attempt++;
			float delay = (float)(1 << (_request_retry_attempt - 1)); // 1s, 2s, 4s
			print_line(vformat("AgenticOrchestrator: transient network failure (%s); retry %d/%d in %.0fs",
					p_error, _request_retry_attempt, MAX_REQUEST_RETRIES, delay));
			_emit_progress_update(vformat("Network issue, retrying (%d/%d)...",
					_request_retry_attempt, MAX_REQUEST_RETRIES), current_run.model_turns);
			_schedule_request_retry(delay);
			return;
		}
		ERR_PRINT(vformat("AgenticOrchestrator: Provider request failed: %s", p_error));
		_emit_run_complete(false, vformat("Error: Model request failed - %s", p_error));
		_is_running = false;
		return;
	}

	// Request succeeded — clear the transient-failure retry counter for the next turn.
	_request_retry_attempt = 0;

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

	// Log raw response for dashboard
	{
		Dictionary tokens;
		if (p_api_response.has("usage")) {
			tokens = p_api_response["usage"];
		}
		int64_t latency_ms = 0;
		Ref<AIProvider> prov = AI::get_singleton()->get_provider();
		if (prov.is_valid()) {
			latency_ms = prov->get_last_request_latency_ms();
		}
		AI::get_singleton()->log_raw_api("response", current_run.model_turns, p_api_response, 200, tokens, latency_ms);
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
	// End-to-end HTTP round trip for this API call — stamped by the provider
	// before it emitted request_completed. Read once and thread it through so
	// the transcript can display it alongside the bubble when Debug is on.
	{
		Ref<AIProvider> prov = AI::get_singleton()->get_provider();
		if (prov.is_valid()) {
			int64_t lat = prov->get_last_request_latency_ms();
			if (lat > 0) {
				assistant_canonical["latency_ms"] = lat;
				assistant_canonical["provider"] = prov->get_provider_name();
			}
		}
	}
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

			// Build capture-time list: prefer `screenshot_times_seconds` (array),
			// fall back to single `wait_seconds`, default to one capture at 2.0s.
			_async_rns_capture_times.clear();
			if (args.has("screenshot_times_seconds")) {
				Array raw = args["screenshot_times_seconds"];
				for (int t = 0; t < raw.size(); t++) {
					Variant v = raw[t];
					float seconds = 0.0f;
					if (v.get_type() == Variant::INT) {
						seconds = (float)(int64_t)v;
					} else if (v.get_type() == Variant::FLOAT) {
						seconds = (float)v;
					} else {
						continue;
					}
					if (seconds <= 0.0f) {
						continue; // Drop non-positive times — capture must come after game starts.
					}
					_async_rns_capture_times.push_back(seconds);
				}
				_async_rns_capture_times.sort();
				// Dedupe while preserving order (sorted, so duplicates are adjacent).
				for (int t = _async_rns_capture_times.size() - 1; t > 0; t--) {
					if (Math::is_equal_approx(_async_rns_capture_times[t], _async_rns_capture_times[t - 1])) {
						_async_rns_capture_times.remove_at(t);
					}
				}
			}
			if (_async_rns_capture_times.is_empty()) {
				float legacy = (float)args.get("wait_seconds", 2.0f);
				if (legacy <= 0.0f) {
					legacy = 2.0f;
				}
				_async_rns_capture_times.push_back(legacy);
			}
			_async_rns_next_capture_index = 0;
			_async_rns_captured_b64s = Array();

			_async_rns_phase = ASYNC_RNS_POLL_START;
			_async_rns_phase_start_ms = Time::get_singleton()->get_ticks_msec();
			_async_rns_action_start_ms = _async_rns_phase_start_ms;
			_async_rns_game_running_ms = 0;

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
			String trd_type = trd.get("type", "");
			if (trd_type == "run_and_screenshot" || trd_type == "capture_2d_viewport" || trd_type == "capture_3d_viewport") {
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

	// Report file-level scene changes from this batch before the next turn.
	_append_batch_scene_diffs();

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

	// Attach images from tool result (e.g. preview_asset thumbnails, capture_*_viewport).
	// Strip the image payload from JSON content to avoid inflating token count as text.
	// Two input shapes are supported:
	//   - result._images: [b64, ...]         — generic multi-image (preview_asset)
	//   - result.screenshot_b64: b64          — single screenshot (capture_*_viewport)
	if (status == "success") {
		Dictionary result_inner = exec_result.get("result", Dictionary());
		bool has_images_field = result_inner.has("_images");
		bool has_screenshot_field = result_inner.has("screenshot_b64");
		if (has_images_field || has_screenshot_field) {
			Array imgs;
			if (has_images_field) {
				imgs = result_inner["_images"];
			}
			if (has_screenshot_field) {
				imgs.push_back(result_inner["screenshot_b64"]);
			}
			message["_images"] = imgs;

			Dictionary clean = exec_result.duplicate();
			Dictionary r = result_inner.duplicate();
			if (has_images_field) {
				r.erase("_images");
			}
			if (has_screenshot_field) {
				r.erase("screenshot_b64");
				r["screenshot"] = "<see attached image>";
			}
			clean["result"] = r;
			message["content"] = JSON::stringify(clean);
		}
	}

	// Attach structured data for UI (not sent to API, just for display)
	message["_tool_result_data"] = tool_result_data;

	return message;
}

// Old validation/legacy methods removed — native tool-calling handles this via the API

// (Old _validate_response, _is_final_response, _execute_actions, _create_tool_result,
//  _create_validation_error_result, _generate_action_id removed — native tool-calling replaces them)

void AgenticOrchestrator::_handle_cancellation() {
	// A cancelled batch may have mutated scenes without a diff being appended.
	// Refresh their snapshots silently so the next run's [SCENE CHANGES] pass
	// doesn't attribute the AI's own changes to the user.
	_refresh_batch_snapshots_silent();
	_emit_run_complete(false, "<turn_cancelled>\nThe previous turn was cancelled by the user. Any in-progress work was stopped. Verify current state before continuing.\n</turn_cancelled>");
	_is_running = false;
	_waiting_for_response = false;
}

// Formats one scene's diff entry for a context block. Empty result = nothing
// worth telling the model.
String AgenticOrchestrator::_format_scene_diff_entry(const Dictionary &p_diff) {
	String path = p_diff.get("path", "");
	if (path.is_empty()) {
		return String();
	}
	if ((bool)p_diff.get("missing", false)) {
		return vformat("%s: no longer exists (deleted or renamed).", path);
	}
	if ((bool)p_diff.get("too_large", false)) {
		return vformat("%s: changed substantially (now %d lines, was %d) — too large to show inline. Use read_scene_file to see the current state.",
				path, (int)p_diff.get("new_lines", 0), (int)p_diff.get("old_lines", 0));
	}
	String diff_text = p_diff.get("diff", "");
	if (diff_text.is_empty()) {
		return String();
	}
	return vformat("%s (+%d/-%d lines):\n%s", path,
			(int)p_diff.get("added", 0), (int)p_diff.get("removed", 0), diff_text.strip_edges());
}

void AgenticOrchestrator::_append_batch_scene_diffs() {
	Vector<String> paths = AISceneDiff::take_batch_scenes();
	if (paths.is_empty()) {
		return;
	}

	String block;
	Array changed_scenes;
	for (const String &scene_path : paths) {
		if (!AISceneDiff::has_snapshot(scene_path)) {
			// First sight of this scene (e.g. just created): store a baseline,
			// nothing to diff against yet.
			String text = AISceneDiff::serialize_open_scene(scene_path);
			if (text.is_empty()) {
				text = AISceneDiff::read_disk_scene(scene_path);
			}
			if (!text.is_empty()) {
				AISceneDiff::set_snapshot(scene_path, text);
			}
			continue;
		}
		Dictionary diff = AISceneDiff::diff_scene_against_snapshot(scene_path);
		if (!(bool)diff.get("changed", false) && !(bool)diff.get("missing", false)) {
			continue;
		}
		String entry = _format_scene_diff_entry(diff);
		if (entry.is_empty()) {
			continue;
		}
		block += entry + "\n";
		changed_scenes.push_back(diff);
	}
	if (block.is_empty()) {
		return;
	}

	Dictionary diff_msg;
	diff_msg["role"] = "user";
	diff_msg["content"] = "[SCENE UPDATE] Resulting scene file changes from your tool calls this turn:\n\n" + block.strip_edges();
	current_run.conversation_history.push_back(diff_msg);
	current_run.run_messages.push_back(diff_msg);

	Dictionary info;
	info["attribution"] = "ai";
	info["scenes"] = changed_scenes;
	emit_signal("scene_diff_ready", info);
}

void AgenticOrchestrator::_refresh_batch_snapshots_silent() {
	Vector<String> paths = AISceneDiff::take_batch_scenes();
	for (const String &scene_path : paths) {
		if (AISceneDiff::has_snapshot(scene_path)) {
			AISceneDiff::diff_scene_against_snapshot(scene_path); // Discard; updates snapshot.
		}
	}
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
			_async_rns_game_running_ms = now_ms;
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
		// Time to next capture is measured from when the game was first observed
		// running, so a slow capture doesn't push the next deadline back.
		float elapsed = (now_ms - _async_rns_game_running_ms) / 1000.0f;
		float next_deadline = (_async_rns_next_capture_index < _async_rns_capture_times.size())
				? _async_rns_capture_times[_async_rns_next_capture_index]
				: 0.0f;
		print_line(vformat("AI_RNS: WAIT_VISUAL elapsed=%.2fs / %.2fs (capture %d of %d)",
				elapsed, next_deadline,
				_async_rns_next_capture_index + 1, _async_rns_capture_times.size()));
		if (elapsed >= next_deadline) {
			// Time to capture. Connect one-shot listener then trigger.
			_async_rns_phase = ASYNC_RNS_AWAIT_CAPTURE;
			_async_rns_phase_start_ms = now_ms;

			Callable cb = callable_mp(this, &AgenticOrchestrator::_on_async_rns_capture_received);
			ai->connect("game_screenshot_ready", cb, CONNECT_ONE_SHOT);
			print_line(vformat("AI_RNS: triggering game screenshot %d of %d at t=%.2fs",
					_async_rns_next_capture_index + 1, _async_rns_capture_times.size(), elapsed));
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
	const int idx = _async_rns_next_capture_index;
	const float at_seconds = (idx < _async_rns_capture_times.size())
			? _async_rns_capture_times[idx]
			: 0.0f;
	print_line(vformat("AI_RNS: capture %d of %d received, b64 len=%d, at=%.2fs",
			idx + 1, _async_rns_capture_times.size(), p_b64.length(), at_seconds));

	AI *ai = AI::get_singleton();

	// Empty payload aborts the whole sequence — partial results aren't useful when
	// the model expects all requested timestamps.
	if (p_b64.is_empty()) {
		_async_rns_phase = ASYNC_RNS_INACTIVE;
		if (ai) {
			ai->call("stop_game");
		}
		Dictionary exec_result;
		exec_result["status"] = "error";
		Dictionary ed; ed["code"] = "operation_failed";
		ed["message"] = vformat("Screenshot capture %d of %d failed: empty image.", idx + 1, _async_rns_capture_times.size());
		ed["details"] = Dictionary();
		exec_result["error"] = ed;
		_on_async_rns_complete(exec_result);
		return;
	}

	// Record this capture and decide whether to continue or finalize.
	Dictionary entry;
	entry["at_seconds"] = at_seconds;
	entry["screenshot_b64"] = p_b64;
	_async_rns_captured_b64s.push_back(entry);
	_async_rns_next_capture_index++;

	if (_async_rns_next_capture_index < _async_rns_capture_times.size()) {
		// More captures pending — keep the game running and return to WAIT_VISUAL.
		_async_rns_phase = ASYNC_RNS_WAIT_VISUAL;
		_async_rns_phase_start_ms = Time::get_singleton()->get_ticks_msec();
		_schedule_rns_tick(0.05f);
		return;
	}

	// All captures complete — stop the game and finalize.
	_async_rns_phase = ASYNC_RNS_INACTIVE;
	if (ai) {
		ai->call("stop_game");
	}

	Dictionary exec_result;
	exec_result["status"] = "success";
	Dictionary rd;
	rd["format"] = "png";
	if (_async_rns_captured_b64s.size() == 1) {
		// Single-capture (legacy) shape: keep `screenshot_b64` at top level so the
		// existing chat UI screenshot widget continues to find it.
		Dictionary first = _async_rns_captured_b64s[0];
		rd["screenshot_b64"] = first.get("screenshot_b64", String());
		rd["at_seconds"] = first.get("at_seconds", 0.0f);
	} else {
		// Multi-capture shape: array of {at_seconds, screenshot_b64}.
		rd["screenshots"] = _async_rns_captured_b64s;
		rd["screenshot_count"] = _async_rns_captured_b64s.size();
	}
	exec_result["result"] = rd;
	_on_async_rns_complete(exec_result);
}

void AgenticOrchestrator::_on_async_rns_complete(const Dictionary &p_exec_result) {
	// Wall-clock time from tool invocation to terminal state (capture or timeout).
	// Lets the AI distinguish an immediate crash (elapsed ~= 8000 = POLL_START timeout)
	// from a normal run (elapsed ~= wait_seconds*1000) from a capture hang.
	uint64_t elapsed_to_screenshot_ms = Time::get_singleton()->get_ticks_msec() - _async_rns_action_start_ms;

	// Capture errors and game output (errors clear on next launch, not on stop).
	Array game_errors;
	String game_output;
	EditorDebuggerNode *edn = EditorDebuggerNode::get_singleton();
	if (edn) {
		ScriptEditorDebugger *dbg = edn->get_default_debugger();
		if (dbg) {
			game_errors = dbg->get_structured_errors(20, 8);
		}
	}
	EditorLog *editor_log = EditorNode::get_log();
	if (editor_log) {
		game_output = editor_log->get_recent_messages_text(200);
	}
	if (game_errors.size() > 0) {
		AI *ai_flag = AI::get_singleton();
		if (ai_flag) {
			ai_flag->set_errors_consumed_by_tool(true);
		}
	}

	// Include the main scene path so the model knows which scene was actually run.
	String main_scene = ProjectSettings::get_singleton()->get_setting("application/run/main_scene", "");

	// Enrich the exec result with errors and game output.
	Dictionary enriched = p_exec_result.duplicate();
	String status = enriched.get("status", "error");
	if (status == "success") {
		Dictionary rd = ((Dictionary)enriched.get("result", Dictionary())).duplicate();
		if (!main_scene.is_empty()) {
			rd["main_scene"] = main_scene;
		}
		if (game_errors.size() > 0) {
			rd["game_crashed"] = true;
			rd["errors"] = game_errors;
		}
		if (!game_output.is_empty()) {
			rd["game_output"] = game_output;
		}
		rd["elapsed_to_screenshot_ms"] = (int64_t)elapsed_to_screenshot_ms;
		enriched["result"] = rd;
	} else {
		// Even on tool error (timeout), include game errors — they explain why it timed out.
		Dictionary ed = ((Dictionary)enriched.get("error", Dictionary())).duplicate();
		if (game_errors.size() > 0) {
			ed["game_errors"] = game_errors;
		}
		if (!game_output.is_empty()) {
			ed["game_output"] = game_output;
		}
		ed["elapsed_to_screenshot_ms"] = (int64_t)elapsed_to_screenshot_ms;
		enriched["error"] = ed;
	}

	// Build tool result directly (don't re-execute — we already have the result from async capture)
	Dictionary tool_result_data;
	tool_result_data["tool_name"] = "run_and_screenshot";
	tool_result_data["action_id"] = _async_rns_tool_call_id;
	tool_result_data["type"] = "run_and_screenshot";
	tool_result_data["args"] = _async_rns_action_args;

	tool_result_data["status"] = status;
	if (status == "success") {
		tool_result_data["result"] = enriched.get("result", Dictionary());
	} else {
		tool_result_data["error"] = enriched.get("error", Dictionary());
	}

	// Build native tool result message.
	// Strip every base64 from the JSON content (it inflates the token count as text)
	// and attach the images as _images instead so providers can send them as real
	// image parts. Handles both single-shot (`screenshot_b64`) and multi-shot
	// (`screenshots:[{at_seconds, screenshot_b64}, ...]`) result shapes.
	Dictionary content_for_wire = enriched;
	Array attached_images;
	if (status == "success") {
		Dictionary result = enriched.get("result", Dictionary());
		bool needs_strip = result.has("screenshot_b64") || result.has("screenshots");
		if (needs_strip) {
			content_for_wire = enriched.duplicate();
			Dictionary r = result.duplicate();
			if (r.has("screenshot_b64")) {
				attached_images.push_back(r["screenshot_b64"]);
				r.erase("screenshot_b64");
				r["screenshot"] = "<see attached image>";
			}
			if (r.has("screenshots")) {
				Array shots = r["screenshots"];
				Array shots_for_wire;
				for (int i = 0; i < shots.size(); i++) {
					Dictionary entry = shots[i];
					Dictionary stripped = entry.duplicate();
					if (stripped.has("screenshot_b64")) {
						attached_images.push_back(stripped["screenshot_b64"]);
						stripped.erase("screenshot_b64");
					}
					stripped["screenshot"] = vformat("<see attached image %d>", i + 1);
					shots_for_wire.push_back(stripped);
				}
				r["screenshots"] = shots_for_wire;
			}
			content_for_wire["result"] = r;
		}
	}

	Dictionary message;
	message["role"] = "tool";
	message["tool_call_id"] = _async_rns_tool_call_id;
	message["content"] = JSON::stringify(content_for_wire);
	message["_tool_result_data"] = tool_result_data;

	if (!attached_images.is_empty()) {
		message["_images"] = attached_images;
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

	// Tools that ran before run_and_screenshot in this batch may have mutated scenes.
	_append_batch_scene_diffs();

	_send_model_request();
}
