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
	// Streaming extensions beyond the orchestrator contract (harness-only).
	ADD_SIGNAL(MethodInfo("assistant_delta", PropertyInfo(Variant::STRING, "delta")));
	ADD_SIGNAL(MethodInfo("thinking_delta", PropertyInfo(Variant::STRING, "delta")));
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
	pending_phase_request = _send_request("thread/start", params);
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
		}
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

	bool success = String(result.get("status", "")) == "success";

	// Image-returning tools (capture_*_viewport, preview_asset, screenshots):
	// same result contract the legacy loop used — result._images: [b64, ...]
	// and/or result.screenshot_b64. Strip the payloads from the JSON text
	// (base64 inflates token count) and attach them as real image content.
	Array image_b64s;
	if (success) {
		Dictionary result_inner = result.get("result", Dictionary());
		bool has_images_field = result_inner.has("_images");
		bool has_screenshot_field = result_inner.has("screenshot_b64");
		if (has_images_field || has_screenshot_field) {
			if (has_images_field) {
				image_b64s = result_inner["_images"];
			}
			if (has_screenshot_field) {
				image_b64s.push_back(result_inner["screenshot_b64"]);
			}
			Dictionary clean = result.duplicate();
			Dictionary r = result_inner.duplicate();
			if (has_images_field) {
				r.erase("_images");
			}
			if (has_screenshot_field) {
				r.erase("screenshot_b64");
				r["screenshot"] = "<see attached image>";
			}
			clean["result"] = r;
			result = clean;
		}
	}

	// Emit the panel/store payload in the exact shape the legacy loop used
	// (matched to the pending tool card by action_id == the codex call id).
	{
		Dictionary trd;
		trd["tool_name"] = tool;
		trd["action_id"] = p_params.get("callId", "");
		trd["type"] = tool;
		Dictionary display_args = args;
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
		if (tool == "run_and_screenshot" || tool == "capture_2d_viewport" || tool == "capture_3d_viewport") {
			trd["tokens"] = 1000;
		} else {
			trd["tokens"] = (int64_t)(JSON::stringify(result).length() / 4);
		}
		emit_signal("tool_result_ready", trd);
	}

	Dictionary text_item;
	text_item["type"] = "inputText";
	text_item["text"] = JSON::stringify(result);
	Array content_items;
	content_items.push_back(text_item);
	for (int i = 0; i < image_b64s.size(); i++) {
		Dictionary img_item;
		img_item["type"] = "inputImage";
		img_item["imageUrl"] = "data:image/png;base64," + String(image_b64s[i]);
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
