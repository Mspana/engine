/**************************************************************************/
/*  codex_harness_driver.cpp                                              */
/**************************************************************************/

#include "codex_harness_driver.h"

#include "../ai.h"
#include "../ai_provider.h"
#include "responses_translator.h"

#include "core/config/project_settings.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/string/print_string.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/script_editor_debugger.h"
#include "editor/editor_log.h"
#include "editor/editor_node.h"
#include "scene/main/scene_tree.h"

/* -------------------------------------------------------------------- */
/*  Bindings                                                             */
/* -------------------------------------------------------------------- */

void CodexHarnessDriver::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start_session"), &CodexHarnessDriver::start_session);
	ClassDB::bind_method(D_METHOD("shutdown"), &CodexHarnessDriver::shutdown);
	ClassDB::bind_method(D_METHOD("send_user_message", "text", "user_message_id"), &CodexHarnessDriver::send_user_message);
	ClassDB::bind_method(D_METHOD("steer", "text"), &CodexHarnessDriver::steer);
	ClassDB::bind_method(D_METHOD("cancel_run"), &CodexHarnessDriver::cancel_run);
	ClassDB::bind_method(D_METHOD("is_running"), &CodexHarnessDriver::is_running);
	ClassDB::bind_method(D_METHOD("is_session_ready"), &CodexHarnessDriver::is_session_ready);
	ClassDB::bind_method(D_METHOD("set_resume_thread_id", "thread_id"), &CodexHarnessDriver::set_resume_thread_id);
	ClassDB::bind_method(D_METHOD("get_thread_id"), &CodexHarnessDriver::get_thread_id);
	ClassDB::bind_method(D_METHOD("_handle_frame", "frame"), &CodexHarnessDriver::_handle_frame);
	ClassDB::bind_method(D_METHOD("_handle_child_exit"), &CodexHarnessDriver::_handle_child_exit);

	// Mirror of AgenticOrchestrator's contract so AIStatusPanel can swap loops.
	ADD_SIGNAL(MethodInfo("run_started"));
	ADD_SIGNAL(MethodInfo("api_round_started", PropertyInfo(Variant::INT, "turn")));
	ADD_SIGNAL(MethodInfo("progress_update", PropertyInfo(Variant::STRING, "status"), PropertyInfo(Variant::INT, "turn")));
	ADD_SIGNAL(MethodInfo("assistant_item_ready", PropertyInfo(Variant::DICTIONARY, "item")));
	ADD_SIGNAL(MethodInfo("tool_result_ready", PropertyInfo(Variant::DICTIONARY, "tool_result")));
	ADD_SIGNAL(MethodInfo("turn_tokens_ready", PropertyInfo(Variant::INT, "tokens")));
	ADD_SIGNAL(MethodInfo("run_complete", PropertyInfo(Variant::BOOL, "success"), PropertyInfo(Variant::STRING, "final_message")));
	ADD_SIGNAL(MethodInfo("checkpoint_recommended", PropertyInfo(Variant::INT, "user_message_id")));
	ADD_SIGNAL(MethodInfo("todos_updated", PropertyInfo(Variant::ARRAY, "todos")));
	// Streaming extensions beyond the orchestrator contract (harness-only).
	ADD_SIGNAL(MethodInfo("assistant_delta", PropertyInfo(Variant::STRING, "delta")));
	ADD_SIGNAL(MethodInfo("thinking_delta", PropertyInfo(Variant::STRING, "delta")));
	ADD_SIGNAL(MethodInfo("thinking_done"));
}

void CodexHarnessDriver::_smoke(const String &p_line) {
	if (smoke_mode) {
		print_line("[harness] " + p_line);
	}
}

/* -------------------------------------------------------------------- */
/*  Session lifecycle                                                    */
/* -------------------------------------------------------------------- */

bool CodexHarnessDriver::start_session() {
	if (child_pid != 0) {
		return true;
	}
	smoke_mode = !OS::get_singleton()->get_environment("ARISTOTLE_HARNESS_SMOKE").is_empty();

	String exe_dir = OS::get_singleton()->get_executable_path().get_base_dir();
	String codex_exe = OS::get_singleton()->get_environment("ARISTOTLE_CODEX_EXE");
	if (codex_exe.is_empty()) {
		// Dev default: the pinned spike binary.
		codex_exe = exe_dir.path_join("../modules/ai/harness_spike/bin/codex-x86_64-pc-windows-msvc.exe");
	}
	if (!FileAccess::exists(codex_exe)) {
		ERR_PRINT(vformat("CodexHarnessDriver: codex binary not found at '%s' (set ARISTOTLE_CODEX_EXE).", codex_exe));
		return false;
	}

	// Child environment. The driver owns these unless the user overrode them.
	if (OS::get_singleton()->get_environment("CODEX_HOME").is_empty()) {
		OS::get_singleton()->set_environment("CODEX_HOME", exe_dir.path_join("../modules/ai/harness_spike/codex_home"));
	}
	if (OS::get_singleton()->get_environment("LITELLM_SPIKE_KEY").is_empty()) {
		// config.toml's env_key for the local translator: codex requires the
		// variable to exist; the translator ignores the value.
		OS::get_singleton()->set_environment("LITELLM_SPIKE_KEY", "sk-aristotle-local");
	}

	// Default provider is the in-process translator: make sure it is up.
	String provider = OS::get_singleton()->get_environment("ARISTOTLE_HARNESS_PROVIDER");
	if (provider.is_empty() || provider == "aristotle") {
		String port_str = OS::get_singleton()->get_environment("ARISTOTLE_TRANSLATOR_PORT");
		int port = port_str.is_empty() ? 4123 : port_str.to_int();
		AIResponsesTranslator::get_singleton()->start(port);
	}

	List<String> args;
	args.push_back("app-server");
	// Blocking pipes: full-frame writes guaranteed (see header note).
	Dictionary pipe_info = OS::get_singleton()->execute_with_pipe(codex_exe, args, true);
	if (!pipe_info.has("stdio") || !pipe_info.has("pid")) {
		ERR_PRINT("CodexHarnessDriver: execute_with_pipe failed to spawn codex app-server.");
		return false;
	}
	stdio = pipe_info["stdio"];
	stderr_pipe = pipe_info["stderr"];
	child_pid = (OS::ProcessID)(int64_t)pipe_info["pid"];
	child_alive.set();
	reader_exit.clear();
	reader_thread.start(_reader_thread_func, this);
	stderr_thread.start(_stderr_thread_func, this);
	_smoke(vformat("spawned codex app-server (pid %d)", (int64_t)child_pid));

	phase = PHASE_INITIALIZING;
	Dictionary client_info;
	client_info["name"] = "aristotle-editor";
	client_info["title"] = "Aristotle";
	client_info["version"] = "0.1.0";
	Dictionary caps;
	caps["experimentalApi"] = true; // dynamicTools.
	Dictionary params;
	params["clientInfo"] = client_info;
	params["capabilities"] = caps;
	pending_phase_request = _send_request("initialize", params);
	return true;
}

void CodexHarnessDriver::shutdown() {
	if (child_pid == 0) {
		return;
	}
	reader_exit.set();
	// Killing the child breaks the pipes, unblocking both reader threads.
	if (child_alive.is_set()) {
		OS::get_singleton()->kill(child_pid);
	}
	if (reader_thread.is_started()) {
		reader_thread.wait_to_finish();
	}
	if (stderr_thread.is_started()) {
		stderr_thread.wait_to_finish();
	}
	stdio.unref();
	stderr_pipe.unref();
	child_pid = 0;
	phase = PHASE_IDLE;
	session_ready = false;
	turn_active = false;
}

void CodexHarnessDriver::_handle_child_exit() {
	child_alive.clear();
	// A held-open tool call can't be answered anymore; drop the state.
	rns_phase = RNS_INACTIVE;
	rns_request_id = -1;
	if (turn_active) {
		turn_active = false;
		emit_signal("run_complete", false, "The agent process exited unexpectedly.");
	}
	session_ready = false;
	phase = PHASE_IDLE;
	_smoke("codex process exited");
}

/* -------------------------------------------------------------------- */
/*  Reader thread: JSONL frames -> main thread                           */
/* -------------------------------------------------------------------- */

void CodexHarnessDriver::_reader_thread_func(void *p_userdata) {
	static_cast<CodexHarnessDriver *>(p_userdata)->_reader_loop();
}

void CodexHarnessDriver::_stderr_thread_func(void *p_userdata) {
	static_cast<CodexHarnessDriver *>(p_userdata)->_stderr_loop();
}

void CodexHarnessDriver::_reader_loop() {
	String pending;
	uint8_t chunk[4096];

	while (!reader_exit.is_set()) {
		// Blocking read: returns as soon as any bytes are available, 0 on a
		// broken pipe (child exited or killed).
		uint64_t read = stdio.is_valid() ? stdio->get_buffer(chunk, sizeof(chunk)) : 0;
		if (read == 0) {
			if (reader_exit.is_set() || !OS::get_singleton()->is_process_running(child_pid)) {
				if (!reader_exit.is_set()) {
					call_deferred("_handle_child_exit");
				}
				return;
			}
			OS::get_singleton()->delay_usec(2000);
			continue;
		}
		pending += String::utf8((const char *)chunk, read);
		int nl;
		while ((nl = pending.find("\n")) >= 0) {
			String line = pending.substr(0, nl).strip_edges();
			pending = pending.substr(nl + 1);
			if (!line.is_empty()) {
				call_deferred("_handle_frame", line);
			}
		}
	}
}

void CodexHarnessDriver::_stderr_loop() {
	// Dedicated drain so codex can never stall on a full stderr pipe.
	uint8_t drain[4096];
	while (!reader_exit.is_set()) {
		uint64_t read = stderr_pipe.is_valid() ? stderr_pipe->get_buffer(drain, sizeof(drain)) : 0;
		if (read == 0) {
			if (reader_exit.is_set() || !OS::get_singleton()->is_process_running(child_pid)) {
				return;
			}
			OS::get_singleton()->delay_usec(5000);
			continue;
		}
		if (smoke_mode) {
			String err_text = String::utf8((const char *)drain, read).strip_edges();
			if (!err_text.is_empty()) {
				print_line("[harness stderr] " + err_text.substr(0, 400));
			}
		}
	}
}

/* -------------------------------------------------------------------- */
/*  Frame dispatch (main thread)                                         */
/* -------------------------------------------------------------------- */

void CodexHarnessDriver::_handle_frame(const String &p_frame) {
	Variant parsed = JSON::parse_string(p_frame);
	if (parsed.get_type() != Variant::DICTIONARY) {
		return;
	}
	Dictionary msg = parsed;
	if (msg.has("method")) {
		if (msg.has("id")) {
			_handle_server_request((int)(int64_t)msg["id"], msg["method"], msg.get("params", Dictionary()));
		} else {
			_handle_notification(msg["method"], msg.get("params", Dictionary()));
		}
	} else if (msg.has("id")) {
		_handle_response((int)(int64_t)msg["id"], msg.get("result", Dictionary()), msg.get("error", Dictionary()));
	}
}

void CodexHarnessDriver::_handle_response(int p_id, const Dictionary &p_result, const Dictionary &p_error) {
	if (p_id == pending_phase_request) {
		pending_phase_request = -1;
		if (!p_error.is_empty()) {
			if (phase == PHASE_STARTING_THREAD && !resume_thread_id.is_empty()) {
				// The saved thread is gone (deleted rollout, codex upgrade...):
				// fall back to a fresh thread rather than failing the session.
				print_line("CodexHarnessDriver: thread/resume failed, starting fresh thread");
				resume_thread_id = String();
				_start_thread_request();
				return;
			}
			ERR_PRINT("CodexHarnessDriver: session bring-up failed: " + JSON::stringify(p_error));
			_smoke("bring-up error: " + JSON::stringify(p_error));
			return;
		}
		_advance_phase(p_result);
		return;
	}
	if (p_id == pending_turn_request) {
		// turn/start's response arrives when the turn ends; the notification
		// stream is the source of truth, nothing to do here.
		pending_turn_request = -1;
		return;
	}
}

void CodexHarnessDriver::_advance_phase(const Dictionary &p_result) {
	switch (phase) {
		case PHASE_INITIALIZING: {
			_send_notification("initialized", Dictionary());
			phase = PHASE_CHECKING_ACCOUNT;
			Dictionary params;
			params["refreshToken"] = false;
			pending_phase_request = _send_request("account/read", params);
		} break;

		case PHASE_CHECKING_ACCOUNT: {
			bool logged_in = p_result.has("account") && p_result["account"].get_type() == Variant::DICTIONARY && !Dictionary(p_result["account"]).is_empty();
			if (logged_in) {
				_smoke("already authenticated");
				_start_thread_request();
			} else {
				String key = AIResponsesTranslator::load_key("OPENAI_API_KEY");
				if (key.is_empty()) {
					// Custom providers carry their own env_key auth; proceed.
					_smoke("no OpenAI key; continuing unauthenticated (custom provider)");
					_start_thread_request();
				} else {
					phase = PHASE_LOGGING_IN;
					Dictionary params;
					params["type"] = "apiKey";
					params["apiKey"] = key;
					pending_phase_request = _send_request("account/login/start", params);
				}
			}
		} break;

		case PHASE_LOGGING_IN: {
			_smoke("logged in via apiKey");
			_start_thread_request();
		} break;

		case PHASE_STARTING_THREAD: {
			Dictionary thread = p_result.get("thread", Dictionary());
			thread_id = thread.get("id", "");
			if (thread_id.is_empty()) {
				ERR_PRINT("CodexHarnessDriver: thread/start returned no thread id.");
				return;
			}
			phase = PHASE_READY;
			session_ready = true;
			_smoke("session ready, thread " + thread_id);
			if (!queued_message.is_empty()) {
				String msg = queued_message;
				queued_message = String();
				int64_t id = queued_message_id;
				queued_message_id = 0;
				active_user_message_id = id;
				_start_turn(msg);
			}
		} break;

		default:
			break;
	}
}

void CodexHarnessDriver::_start_thread_request() {
	phase = PHASE_STARTING_THREAD;
	Dictionary params;
	params["cwd"] = ProjectSettings::get_singleton()->globalize_path("res://");
	params["developerInstructions"] = _developer_instructions();
	params["dynamicTools"] = _build_dynamic_tools();
	String model = OS::get_singleton()->get_environment("ARISTOTLE_HARNESS_MODEL");
	params["model"] = model.is_empty() ? String("kimi-k2.6") : model;
	String provider = OS::get_singleton()->get_environment("ARISTOTLE_HARNESS_PROVIDER");
	params["modelProvider"] = provider.is_empty() ? String("aristotle") : provider;
	if (!resume_thread_id.is_empty()) {
		// Resume the chat's existing codex thread: history survives editor
		// restarts. Overrides (tools/instructions/model) are re-applied so a
		// changed toolset isn't stale-restored from the rollout.
		params["threadId"] = resume_thread_id;
		pending_phase_request = _send_request("thread/resume", params);
	} else {
		pending_phase_request = _send_request("thread/start", params);
	}
}

String CodexHarnessDriver::_developer_instructions() {
	// Increment 1: minimal persona. The full guidance from system_prompt.inc
	// gets ported here (persona + editor conventions) in a later increment;
	// codex's own base prompt handles agentic mechanics.
	return String(
			"You are Aristotle, an AI game development assistant embedded in a Godot 4 "
			"engine editor. The editor exposes tools for scene editing, script editing, "
			"running the game, and capturing screenshots — prefer those tools over shell "
			"commands or direct file edits for anything they cover. Scene files and "
			"project state are live in the editor; do not modify .tscn/.tres files "
			"directly with file tools.");
}

Array CodexHarnessDriver::_build_dynamic_tools() {
	// tools_array.inc builds OpenAI-style function entries; convert to codex's
	// DynamicToolFunctionSpec {type, name, description, inputSchema}.
	Array specs;
	Array tools = AIProvider::build_tools_array();
	for (int i = 0; i < tools.size(); i++) {
		if (tools[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		Dictionary t = tools[i];
		Dictionary fn = t.has("function") ? Dictionary(t["function"]) : t;
		String name = fn.get("name", "");
		if (name.is_empty()) {
			continue;
		}
		if (name == "update_todos") {
			// Legacy: codex's native plan tool (turn/plan/updated) drives the
			// todo panel instead.
			continue;
		}
		Dictionary spec;
		spec["type"] = "function";
		spec["name"] = name;
		spec["description"] = fn.get("description", "");
		spec["inputSchema"] = fn.get("parameters", Dictionary());
		specs.push_back(spec);
	}
	_smoke(vformat("declaring %d dynamic tools", specs.size()));
	return specs;
}

/* -------------------------------------------------------------------- */
/*  Turns                                                                */
/* -------------------------------------------------------------------- */

void CodexHarnessDriver::send_user_message(const String &p_text, int64_t p_user_message_id) {
	if (!session_ready) {
		queued_message = p_text;
		queued_message_id = p_user_message_id;
		_smoke("queued message until session is ready");
		return;
	}
	if (turn_active) {
		// Mid-run message: steer the in-flight turn (the panel decides whether
		// to queue or steer; this mirrors inject_user_message semantics).
		steer(p_text);
		return;
	}
	active_user_message_id = p_user_message_id;
	_start_turn(p_text);
}

void CodexHarnessDriver::_start_turn(const String &p_text) {
	run_generation++;
	turn_active = true;
	assistant_text_accum = String();
	turn_counter++;
	emit_signal("run_started");
	emit_signal("api_round_started", turn_counter);

	Dictionary text_input;
	text_input["type"] = "text";
	text_input["text"] = p_text;
	Array input;
	input.push_back(text_input);
	Dictionary params;
	params["threadId"] = thread_id;
	params["input"] = input;
	pending_turn_request = _send_request("turn/start", params);
}

void CodexHarnessDriver::steer(const String &p_text) {
	if (!turn_active || current_turn_id.is_empty()) {
		return;
	}
	Dictionary text_input;
	text_input["type"] = "text";
	text_input["text"] = p_text;
	Array input;
	input.push_back(text_input);
	Dictionary params;
	params["threadId"] = thread_id;
	params["expectedTurnId"] = current_turn_id;
	params["input"] = input;
	_send_request("turn/steer", params);
	_smoke("steered in-flight turn");
}

void CodexHarnessDriver::cancel_run() {
	if (rns_phase != RNS_INACTIVE || rns_request_id >= 0) {
		// Stop the game and answer the held-open call so codex isn't left
		// waiting on a tool that will never respond.
		_abort_async_rns("user_cancelled_run");
	}
	if (!turn_active || current_turn_id.is_empty()) {
		return;
	}
	run_generation++;
	Dictionary params;
	params["threadId"] = thread_id;
	params["turnId"] = current_turn_id;
	_send_request("turn/interrupt", params);
	_smoke("interrupt requested");
}

/* -------------------------------------------------------------------- */
/*  Notifications                                                        */
/* -------------------------------------------------------------------- */

void CodexHarnessDriver::_handle_notification(const String &p_method, const Dictionary &p_params) {
	if (p_method == "turn/started") {
		Dictionary turn = p_params.get("turn", Dictionary());
		current_turn_id = turn.get("id", "");
		emit_signal("progress_update", "Working...", turn_counter);
		_smoke("turn started " + current_turn_id);
		return;
	}
	if (p_method == "item/agentMessage/delta") {
		String delta = p_params.get("delta", "");
		if (!delta.is_empty()) {
			emit_signal("assistant_delta", delta);
		}
		if (smoke_mode) {
			OS::get_singleton()->print("%s", delta.utf8().get_data());
		}
		return;
	}
	if (p_method == "item/reasoning/summaryTextDelta" || p_method == "item/reasoning/textDelta") {
		String delta = p_params.get("delta", "");
		if (!delta.is_empty()) {
			emit_signal("thinking_delta", delta);
		}
		return;
	}
	if (p_method == "item/started") {
		Dictionary item = p_params.get("item", Dictionary());
		if (String(item.get("type", "")) == "dynamicToolCall") {
			// Announce the tool call as a canonical assistant item so the
			// panel creates its pending tool card (resolved by
			// tool_result_ready when item/completed arrives).
			Dictionary block;
			block["type"] = "tool_call";
			block["id"] = item.get("id", "");
			block["name"] = item.get("tool", "");
			Variant args = item.get("arguments", Dictionary());
			block["args"] = args.get_type() == Variant::STRING ? Variant(JSON::parse_string(args)) : args;
			Array content;
			content.push_back(block);
			Dictionary assistant_item;
			assistant_item["role"] = "assistant";
			assistant_item["content"] = content;
			emit_signal("assistant_item_ready", assistant_item);
		}
		return;
	}
	if (p_method == "item/completed") {
		Dictionary item = p_params.get("item", Dictionary());
		String type = item.get("type", "");
		if (type == "agentMessage") {
			String text = item.get("text", "");
			if (!text.is_empty()) {
				assistant_text_accum = text;
				Dictionary block;
				block["type"] = "text";
				block["text"] = text;
				Array content;
				content.push_back(block);
				Dictionary assistant_item;
				assistant_item["role"] = "assistant";
				assistant_item["content"] = content;
				emit_signal("assistant_item_ready", assistant_item);
			}
			if (smoke_mode) {
				OS::get_singleton()->print("\n");
			}
		} else if (type == "dynamicToolCall") {
			// tool_result_ready is emitted from _execute_dynamic_tool, where
			// args and the exec result are in hand (legacy payload shape).
			_smoke(vformat("tool %s -> %s", String(item.get("tool", "")), String(item.get("status", ""))));
		} else if (type == "reasoning") {
			// A reasoning item finished: back-to-back thinking phases must
			// render (and persist) as separate blocks.
			emit_signal("thinking_done");
		}
		return;
	}
	if (p_method == "turn/plan/updated") {
		// Codex's native plan tool drives the panel's todo widget directly
		// (replaces the legacy update_todos tool, which is no longer declared).
		Array plan = p_params.get("plan", Array());
		Array todos;
		for (int i = 0; i < plan.size(); i++) {
			if (plan[i].get_type() != Variant::DICTIONARY) {
				continue;
			}
			Dictionary step = plan[i];
			Dictionary todo;
			todo["id"] = itos(i + 1);
			todo["content"] = step.get("step", "");
			String status = step.get("status", "pending");
			todo["status"] = status == "inProgress" ? String("in_progress") : status;
			todos.push_back(todo);
		}
		emit_signal("todos_updated", todos);
		return;
	}
	if (p_method == "turn/completed") {
		Dictionary turn = p_params.get("turn", Dictionary());
		String status = turn.get("status", "");
		bool success = status == "completed";
		String final_message = assistant_text_accum;
		if (!success) {
			Dictionary error = turn.get("error", Dictionary());
			String err_text = error.get("message", "");
			final_message = err_text.is_empty() ? vformat("Turn ended: %s", status) : err_text;
		}
		turn_active = false;
		current_turn_id = String();
		emit_signal("run_complete", success, final_message);
		if (success && active_user_message_id != 0) {
			emit_signal("checkpoint_recommended", active_user_message_id);
		}
		_smoke("turn completed: " + status);
		return;
	}
}

/* -------------------------------------------------------------------- */
/*  Server requests (approvals, dynamic tool execution)                  */
/* -------------------------------------------------------------------- */

void CodexHarnessDriver::_handle_server_request(int p_id, const String &p_method, const Dictionary &p_params) {
	if (p_method == "item/tool/call") {
		String tool = p_params.get("tool", "");
		if (tool == "run_and_screenshot") {
			// Held-open call: the response is sent when captures complete.
			_begin_async_rns(p_id, p_params);
			return;
		}
		if (tool == "install_export_templates" || tool == "export_project" || tool == "serve_web_build") {
			// Async export machinery not yet ported to harness mode.
			Dictionary err;
			err["code"] = "not_supported_in_harness";
			err["message"] = vformat("'%s' is not available in Codex Harness mode yet. Ask the user to switch to a legacy model for export operations.", tool);
			Dictionary exec_result;
			exec_result["status"] = "error";
			exec_result["error"] = err;
			_send_response(p_id, _tool_response_from_result(tool, Dictionary(), p_params.get("callId", ""), exec_result));
			return;
		}
		_send_response(p_id, _execute_dynamic_tool(p_params));
		return;
	}
	if (p_method.contains("requestApproval")) {
		// Increment 1: no approval UI yet; decline so nothing runs unreviewed.
		Dictionary resp;
		resp["decision"] = "decline";
		_send_response(p_id, resp);
		_smoke("declined approval request: " + p_method);
		return;
	}
	_send_response(p_id, Dictionary());
	_smoke("unhandled server request: " + p_method);
}

Dictionary CodexHarnessDriver::_execute_dynamic_tool(const Dictionary &p_params) {
	String tool = p_params.get("tool", "");
	Variant args_v = p_params.get("arguments", Dictionary());
	Dictionary args;
	if (args_v.get_type() == Variant::STRING) {
		Variant parsed = JSON::parse_string(args_v);
		if (parsed.get_type() == Variant::DICTIONARY) {
			args = parsed;
		}
	} else if (args_v.get_type() == Variant::DICTIONARY) {
		args = args_v;
	}
	_smoke(vformat("executing tool %s", tool));

	Dictionary action;
	action["action"] = tool;
	action["args"] = args;
	Dictionary result = AI::get_singleton()->execute_single_action(action);
	return _tool_response_from_result(tool, args, p_params.get("callId", ""), result);
}

// Shared tail for every dynamic tool response (sync and held-open async):
// emits tool_result_ready for the panel (full result, images included so the
// screenshot widget renders), then builds the codex response with base64
// payloads stripped from the JSON text and attached as inputImage content.
// Image contract (same as the legacy loop): result._images: [b64, ...],
// result.screenshot_b64: b64, result.screenshots: [{at_seconds, screenshot_b64}].
Dictionary CodexHarnessDriver::_tool_response_from_result(const String &p_tool, const Dictionary &p_args, const String &p_call_id, const Dictionary &p_exec_result) {
	Dictionary result = p_exec_result;
	bool success = String(result.get("status", "")) == "success";

	// Panel/store payload, matched to the pending card by action_id.
	{
		Dictionary trd;
		trd["tool_name"] = p_tool;
		trd["action_id"] = p_call_id;
		trd["type"] = p_tool;
		Dictionary display_args = p_args;
		Dictionary result_inner = result.get("result", Dictionary());
		if (success && result_inner.has("_display_args")) {
			display_args = result_inner["_display_args"];
			result_inner.erase("_display_args");
		}
		trd["args"] = display_args;
		trd["status"] = result.get("status", "error");
		if (success) {
			trd["result"] = result_inner;
		} else {
			trd["error"] = result.get("error", Dictionary());
		}
		// Same token heuristic as the legacy loop: captures count as ~1000.
		if (p_tool == "run_and_screenshot" || p_tool == "capture_2d_viewport" || p_tool == "capture_3d_viewport") {
			trd["tokens"] = 1000;
		} else {
			trd["tokens"] = (int64_t)(JSON::stringify(result).length() / 4);
		}
		emit_signal("tool_result_ready", trd);
	}

	Array image_b64s;
	if (success) {
		Dictionary result_inner = result.get("result", Dictionary());
		bool has_images_field = result_inner.has("_images");
		bool has_screenshot_field = result_inner.has("screenshot_b64");
		bool has_screenshots_array = result_inner.has("screenshots");
		if (has_images_field || has_screenshot_field || has_screenshots_array) {
			Dictionary clean = result.duplicate();
			Dictionary r = result_inner.duplicate();
			if (has_images_field) {
				image_b64s = r["_images"];
				r.erase("_images");
			}
			if (has_screenshot_field) {
				image_b64s.push_back(r["screenshot_b64"]);
				r.erase("screenshot_b64");
				r["screenshot"] = "<see attached image>";
			}
			if (has_screenshots_array) {
				// Multi-shot run_and_screenshot shape.
				Array shots = r["screenshots"];
				Array shot_meta;
				for (int i = 0; i < shots.size(); i++) {
					Dictionary shot = shots[i];
					image_b64s.push_back(shot.get("screenshot_b64", String()));
					Dictionary meta;
					meta["at_seconds"] = shot.get("at_seconds", 0.0f);
					meta["screenshot"] = vformat("<see attached image %d>", i + 1);
					shot_meta.push_back(meta);
				}
				r["screenshots"] = shot_meta;
			}
			clean["result"] = r;
			result = clean;
		}
	}

	Dictionary text_item;
	text_item["type"] = "inputText";
	text_item["text"] = JSON::stringify(result);
	Array content_items;
	content_items.push_back(text_item);
	for (int i = 0; i < image_b64s.size(); i++) {
		String b64 = image_b64s[i];
		if (b64.is_empty()) {
			continue;
		}
		Dictionary img_item;
		img_item["type"] = "inputImage";
		img_item["imageUrl"] = "data:image/png;base64," + b64;
		content_items.push_back(img_item);
	}
	if (!image_b64s.is_empty()) {
		_smoke(vformat("attached %d image(s) to tool result", image_b64s.size()));
	}
	Dictionary resp;
	resp["success"] = success;
	resp["contentItems"] = content_items;
	return resp;
}

/* -------------------------------------------------------------------- */
/*  run_and_screenshot: held-open call (ported from AgenticOrchestrator)  */
/* -------------------------------------------------------------------- */

void CodexHarnessDriver::_begin_async_rns(int p_request_id, const Dictionary &p_params) {
	String call_id = p_params.get("callId", "");
	if (rns_phase != RNS_INACTIVE) {
		Dictionary err;
		err["code"] = "operation_failed";
		err["message"] = "run_and_screenshot is already in progress.";
		Dictionary exec_result;
		exec_result["status"] = "error";
		exec_result["error"] = err;
		_send_response(p_request_id, _tool_response_from_result("run_and_screenshot", Dictionary(), call_id, exec_result));
		return;
	}

	Variant args_v = p_params.get("arguments", Dictionary());
	Dictionary args;
	if (args_v.get_type() == Variant::STRING) {
		Variant parsed = JSON::parse_string(args_v);
		if (parsed.get_type() == Variant::DICTIONARY) {
			args = parsed;
		}
	} else if (args_v.get_type() == Variant::DICTIONARY) {
		args = args_v;
	}

	rns_request_id = p_request_id;
	rns_call_id = call_id;
	rns_args = args;

	// Capture-time list: prefer `screenshot_times_seconds`, fall back to
	// single `wait_seconds`, default one capture at 2.0s (legacy rules).
	rns_capture_times.clear();
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
				continue;
			}
			rns_capture_times.push_back(seconds);
		}
		rns_capture_times.sort();
		for (int t = rns_capture_times.size() - 1; t > 0; t--) {
			if (Math::is_equal_approx(rns_capture_times[t], rns_capture_times[t - 1])) {
				rns_capture_times.remove_at(t);
			}
		}
	}
	if (rns_capture_times.is_empty()) {
		float legacy = (float)args.get("wait_seconds", 2.0f);
		if (legacy <= 0.0f) {
			legacy = 2.0f;
		}
		rns_capture_times.push_back(legacy);
	}
	rns_next_capture_index = 0;
	rns_captured = Array();
	rns_phase = RNS_POLL_START;
	rns_phase_start_ms = Time::get_singleton()->get_ticks_msec();
	rns_action_start_ms = rns_phase_start_ms;
	rns_game_running_ms = 0;
	rns_run_generation = run_generation;

	Dictionary action;
	action["action"] = "run_and_screenshot";
	action["args"] = args;
	Dictionary run_result = AI::get_singleton()->execute_single_action(action);
	_smoke(vformat("rns launch status=%s", String(run_result.get("status", "?"))));
	if (String(run_result.get("status", "")) != "success") {
		if (run_result.is_empty()) {
			Dictionary ed;
			ed["code"] = "internal_error";
			ed["message"] = "AI singleton not available to launch the game.";
			run_result["status"] = "error";
			run_result["error"] = ed;
		}
		rns_phase = RNS_INACTIVE;
		int req = rns_request_id;
		rns_request_id = -1;
		_send_response(req, _tool_response_from_result("run_and_screenshot", args, call_id, run_result));
		return;
	}
	emit_signal("progress_update", "Running the game...", turn_counter);
	_schedule_rns_tick(0.1f);
}

void CodexHarnessDriver::_schedule_rns_tick(float p_delay) {
	rns_tick_gen++;
	SceneTree *tree = Object::cast_to<SceneTree>(OS::get_singleton()->get_main_loop());
	ERR_FAIL_NULL(tree);
	Ref<SceneTreeTimer> timer = tree->create_timer(p_delay);
	timer->connect("timeout", callable_mp(this, &CodexHarnessDriver::_rns_tick_gen_cb).bind(rns_tick_gen), CONNECT_ONE_SHOT);
}

void CodexHarnessDriver::_rns_tick_gen_cb(uint32_t p_gen) {
	if (p_gen != rns_tick_gen) {
		return; // Stale timer from a prior schedule.
	}
	_rns_tick();
}

void CodexHarnessDriver::_rns_tick() {
	if (rns_phase == RNS_INACTIVE) {
		return;
	}
	AI *ai = AI::get_singleton();
	uint64_t now_ms = Time::get_singleton()->get_ticks_msec();

	if (rns_phase == RNS_POLL_START) {
		bool game_running = ai && (bool)ai->call("get_game_is_running");
		if (game_running) {
			rns_phase = RNS_WAIT_VISUAL;
			rns_phase_start_ms = now_ms;
			rns_game_running_ms = now_ms;
			_schedule_rns_tick(0.1f);
		} else if (now_ms - rns_phase_start_ms > 8000) {
			rns_phase = RNS_INACTIVE;
			Dictionary err;
			err["status"] = "error";
			Dictionary ed;
			ed["code"] = "operation_failed";
			ed["message"] = "Game did not start within 8 seconds.";
			err["error"] = ed;
			_finish_async_rns(err);
		} else {
			_schedule_rns_tick(0.15f);
		}
	} else if (rns_phase == RNS_WAIT_VISUAL) {
		float elapsed = (now_ms - rns_game_running_ms) / 1000.0f;
		float next_deadline = (rns_next_capture_index < rns_capture_times.size())
				? rns_capture_times[rns_next_capture_index]
				: 0.0f;
		if (elapsed >= next_deadline) {
			rns_phase = RNS_AWAIT_CAPTURE;
			rns_phase_start_ms = now_ms;
			Callable cb = callable_mp(this, &CodexHarnessDriver::_on_rns_capture_received);
			ai->connect("game_screenshot_ready", cb, CONNECT_ONE_SHOT);
			ai->trigger_game_screenshot();
			_schedule_rns_tick(10.0f); // Capture timeout.
		} else {
			_schedule_rns_tick(0.1f);
		}
	} else if (rns_phase == RNS_AWAIT_CAPTURE) {
		Callable cb = callable_mp(this, &CodexHarnessDriver::_on_rns_capture_received);
		if (ai && ai->is_connected("game_screenshot_ready", cb)) {
			ai->disconnect("game_screenshot_ready", cb);
		}
		rns_phase = RNS_INACTIVE;
		Dictionary err;
		err["status"] = "error";
		Dictionary ed;
		ed["code"] = "operation_failed";
		ed["message"] = "Screenshot capture timed out (10s).";
		err["error"] = ed;
		_finish_async_rns(err);
	}
}

void CodexHarnessDriver::_on_rns_capture_received(const String &p_b64) {
	if (rns_phase == RNS_INACTIVE) {
		return;
	}
	const int idx = rns_next_capture_index;
	const float at_seconds = (idx < rns_capture_times.size()) ? rns_capture_times[idx] : 0.0f;
	AI *ai = AI::get_singleton();

	if (p_b64.is_empty()) {
		rns_phase = RNS_INACTIVE;
		if (ai) {
			ai->call("stop_game");
		}
		Dictionary exec_result;
		exec_result["status"] = "error";
		Dictionary ed;
		ed["code"] = "operation_failed";
		ed["message"] = vformat("Screenshot capture %d of %d failed: empty image.", idx + 1, rns_capture_times.size());
		exec_result["error"] = ed;
		_finish_async_rns(exec_result);
		return;
	}

	Dictionary entry;
	entry["at_seconds"] = at_seconds;
	entry["screenshot_b64"] = p_b64;
	rns_captured.push_back(entry);
	rns_next_capture_index++;

	if (rns_next_capture_index < rns_capture_times.size()) {
		rns_phase = RNS_WAIT_VISUAL;
		rns_phase_start_ms = Time::get_singleton()->get_ticks_msec();
		_schedule_rns_tick(0.05f);
		return;
	}

	rns_phase = RNS_INACTIVE;
	if (ai) {
		ai->call("stop_game");
	}
	Dictionary exec_result;
	exec_result["status"] = "success";
	Dictionary rd;
	rd["format"] = "png";
	if (rns_captured.size() == 1) {
		Dictionary first = rns_captured[0];
		rd["screenshot_b64"] = first.get("screenshot_b64", String());
		rd["at_seconds"] = first.get("at_seconds", 0.0f);
	} else {
		rd["screenshots"] = rns_captured;
		rd["screenshot_count"] = rns_captured.size();
	}
	exec_result["result"] = rd;
	_finish_async_rns(exec_result);
}

void CodexHarnessDriver::_finish_async_rns(const Dictionary &p_exec_result) {
	if (rns_request_id < 0) {
		return;
	}
	uint64_t elapsed_to_screenshot_ms = Time::get_singleton()->get_ticks_msec() - rns_action_start_ms;

	// Enrich with game errors and output (same as the legacy loop) — they
	// explain crashes and timeouts to the model.
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
	String requested_scene = rns_args.get("scene_path", String());
	String main_scene = ProjectSettings::get_singleton()->get_setting("application/run/main_scene", "");

	Dictionary enriched = p_exec_result.duplicate();
	if (String(enriched.get("status", "error")) == "success") {
		Dictionary rd = ((Dictionary)enriched.get("result", Dictionary())).duplicate();
		if (!requested_scene.is_empty()) {
			rd["scene_path"] = requested_scene;
		} else if (!main_scene.is_empty()) {
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

	int req = rns_request_id;
	rns_request_id = -1;
	rns_phase = RNS_INACTIVE;
	_send_response(req, _tool_response_from_result("run_and_screenshot", rns_args, rns_call_id, enriched));
}

void CodexHarnessDriver::_abort_async_rns(const String &p_reason) {
	if (rns_phase == RNS_INACTIVE && rns_request_id < 0) {
		return;
	}
	AI *ai = AI::get_singleton();
	Callable cb = callable_mp(this, &CodexHarnessDriver::_on_rns_capture_received);
	if (ai && ai->is_connected("game_screenshot_ready", cb)) {
		ai->disconnect("game_screenshot_ready", cb);
	}
	if (ai) {
		ai->call("stop_game");
	}
	rns_phase = RNS_INACTIVE;
	Dictionary exec_result;
	exec_result["status"] = "cancelled";
	Dictionary ed;
	ed["code"] = "cancelled";
	ed["message"] = p_reason;
	exec_result["error"] = ed;
	_finish_async_rns(exec_result);
}

/* -------------------------------------------------------------------- */
/*  Wire helpers                                                         */
/* -------------------------------------------------------------------- */

int CodexHarnessDriver::_send_request(const String &p_method, const Dictionary &p_params) {
	next_request_id++;
	Dictionary frame;
	frame["id"] = next_request_id;
	frame["method"] = p_method;
	frame["params"] = p_params;
	_write_frame(frame);
	return next_request_id;
}

void CodexHarnessDriver::_send_notification(const String &p_method, const Dictionary &p_params) {
	Dictionary frame;
	frame["method"] = p_method;
	if (!p_params.is_empty()) {
		frame["params"] = p_params;
	}
	_write_frame(frame);
}

void CodexHarnessDriver::_send_response(int p_id, const Dictionary &p_result) {
	Dictionary frame;
	frame["id"] = p_id;
	frame["result"] = p_result;
	_write_frame(frame);
}

void CodexHarnessDriver::_write_frame(const Dictionary &p_frame) {
	if (stdio.is_null()) {
		return;
	}
	CharString line = (JSON::stringify(p_frame) + "\n").utf8();
	stdio->store_buffer((const uint8_t *)line.get_data(), line.length());
	if (smoke_mode) {
		Dictionary redacted = p_frame.duplicate(true);
		if (redacted.has("params") && Dictionary(redacted["params"]).has("apiKey")) {
			Dictionary params = redacted["params"];
			params["apiKey"] = "<redacted>";
			redacted["params"] = params;
		}
		String s = JSON::stringify(redacted);
		print_line("[harness ->] " + (s.length() > 300 ? s.substr(0, 300) + "..." : s));
	}
}
