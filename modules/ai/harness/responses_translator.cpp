/**************************************************************************/
/*  responses_translator.cpp                                              */
/**************************************************************************/

#include "responses_translator.h"

#include "../ai_text_sanitizer.h"
#include "core/crypto/crypto.h"
#include "core/io/file_access.h"
#include "core/io/http_client.h"
#include "core/io/ip_address.h"
#include "core/io/json.h"
#include "core/io/stream_peer_tcp.h"
#include "core/io/tcp_server.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/string/print_string.h"
#include "core/templates/hash_map.h"

static Dictionary _make_error(const String &p_msg) {
	Dictionary err;
	err["message"] = p_msg;
	Dictionary out;
	out["error"] = err;
	return out;
}

AIResponsesTranslator *AIResponsesTranslator::singleton = nullptr;

AIResponsesTranslator *AIResponsesTranslator::get_singleton() {
	if (!singleton) {
		singleton = memnew(AIResponsesTranslator);
	}
	return singleton;
}

/* -------------------------------------------------------------------- */
/*  Upstream registry                                                    */
/* -------------------------------------------------------------------- */

struct UpstreamEntry {
	const char *model;
	const char *host;
	const char *path;
	const char *env_key;
};

// Chat Completions providers reachable through the translator. Models with
// native Responses endpoints (OpenAI, xAI) don't come through here at all -
// codex talks to them directly.
static const UpstreamEntry UPSTREAMS[] = {
	{ "kimi-k2.6", "api.moonshot.ai", "/v1/chat/completions", "MOONSHOT_API_KEY" },
	{ "kimi-k2.5", "api.moonshot.ai", "/v1/chat/completions", "MOONSHOT_API_KEY" },
	{ "kimi-k3", "api.moonshot.ai", "/v1/chat/completions", "MOONSHOT_API_KEY" },
};

String AIResponsesTranslator::load_key(const String &p_env_var) {
	String v = OS::get_singleton()->get_environment(p_env_var);
	if (!v.is_empty()) {
		return v;
	}
	// Dev fallback: search .env next to the executable, up the tree, and in
	// modules/ai/ (same order AIProvider uses; duplicated here so the
	// translator stays free of provider-layer coupling).
	String exe_dir = OS::get_singleton()->get_executable_path().get_base_dir();
	const String candidates[] = {
		exe_dir.path_join(".env"),
		exe_dir.path_join("../.env"),
		exe_dir.path_join("../../.env"),
		exe_dir.path_join("../modules/ai/.env"),
		exe_dir.path_join("../../modules/ai/.env"),
	};
	for (const String &path : candidates) {
		if (!FileAccess::exists(path)) {
			continue;
		}
		Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
		if (f.is_null()) {
			continue;
		}
		while (!f->eof_reached()) {
			String line = f->get_line().strip_edges();
			if (line.begins_with("#") || !line.contains("=")) {
				continue;
			}
			String key = line.get_slice("=", 0).strip_edges();
			if (key == p_env_var) {
				return line.substr(line.find("=") + 1).strip_edges();
			}
		}
	}
	return String();
}

bool AIResponsesTranslator::_resolve_upstream(const String &p_model, String &r_host, String &r_path, String &r_api_key, String &r_err) {
	for (const UpstreamEntry &e : UPSTREAMS) {
		if (p_model == e.model) {
			r_host = e.host;
			r_path = e.path;
			r_api_key = load_key(e.env_key);
			if (r_api_key.is_empty()) {
				r_err = vformat("API key env var '%s' is not set for model '%s'.", e.env_key, p_model);
				return false;
			}
			return true;
		}
	}
	r_err = vformat("Model '%s' is not registered with the Aristotle translator.", p_model);
	return false;
}

/* -------------------------------------------------------------------- */
/*  Lifecycle                                                            */
/* -------------------------------------------------------------------- */

bool AIResponsesTranslator::start(int p_port) {
	if (running.is_set()) {
		return true;
	}
	server.instantiate();
	Error err = server->listen(p_port, IPAddress("127.0.0.1"));
	if (err != OK) {
		ERR_PRINT(vformat("AIResponsesTranslator: failed to listen on 127.0.0.1:%d (error %d)", p_port, err));
		server.unref();
		return false;
	}
	port = p_port;
	exit_flag.clear();
	running.set();
	accept_thread.start(_accept_thread_func, this);
	print_line(vformat("AIResponsesTranslator: listening on 127.0.0.1:%d", p_port));
	return true;
}

void AIResponsesTranslator::stop() {
	if (!running.is_set()) {
		return;
	}
	exit_flag.set();
	if (accept_thread.is_started()) {
		accept_thread.wait_to_finish();
	}
	for (ConnSlot &slot : conn_slots) {
		if (slot.started.is_set()) {
			slot.thread.wait_to_finish();
			slot.started.clear();
		}
	}
	if (server.is_valid()) {
		server->stop();
		server.unref();
	}
	running.clear();
}

void AIResponsesTranslator::_accept_thread_func(void *p_userdata) {
	static_cast<AIResponsesTranslator *>(p_userdata)->_accept_loop();
}

void AIResponsesTranslator::_conn_thread_func(void *p_userdata) {
	ConnContext *ctx = static_cast<ConnContext *>(p_userdata);
	ctx->translator->_serve_connection(ctx->peer);
	ctx->translator->conn_slots[ctx->slot].busy.clear();
	memdelete(ctx);
}

void AIResponsesTranslator::_accept_loop() {
	while (!exit_flag.is_set()) {
		if (!server->is_connection_available()) {
			OS::get_singleton()->delay_usec(5000); // 5ms
			continue;
		}
		Ref<StreamPeerTCP> peer = server->take_connection();
		if (peer.is_null()) {
			continue;
		}
		int slot_idx = -1;
		for (int i = 0; i < MAX_CONNECTIONS; i++) {
			if (!conn_slots[i].busy.is_set()) {
				if (conn_slots[i].started.is_set()) {
					conn_slots[i].thread.wait_to_finish();
					conn_slots[i].started.clear();
				}
				slot_idx = i;
				break;
			}
		}
		if (slot_idx < 0) {
			// All slots busy: refuse politely rather than queueing behind a
			// potentially minutes-long streaming turn.
			CharString busy = String("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n").utf8();
			peer->put_data((const uint8_t *)busy.get_data(), busy.length());
			continue;
		}
		ConnContext *ctx = memnew(ConnContext);
		ctx->translator = this;
		ctx->peer = peer;
		ctx->slot = slot_idx;
		conn_slots[slot_idx].busy.set();
		conn_slots[slot_idx].started.set();
		conn_slots[slot_idx].thread.start(_conn_thread_func, ctx);
	}
}

/* -------------------------------------------------------------------- */
/*  HTTP plumbing                                                        */
/* -------------------------------------------------------------------- */

bool AIResponsesTranslator::_client_alive(Ref<StreamPeerTCP> p_client) {
	p_client->poll();
	return p_client->get_status() == StreamPeerTCP::STATUS_CONNECTED;
}

bool AIResponsesTranslator::_send_raw(Ref<StreamPeerTCP> p_client, const CharString &p_data) {
	if (!_client_alive(p_client)) {
		return false;
	}
	return p_client->put_data((const uint8_t *)p_data.get_data(), p_data.length()) == OK;
}

bool AIResponsesTranslator::_send_sse_event(Ref<StreamPeerTCP> p_client, const Dictionary &p_event) {
	String line = "data: " + JSON::stringify(p_event) + "\n\n";
	return _send_raw(p_client, line.utf8());
}

void AIResponsesTranslator::_send_http_json(Ref<StreamPeerTCP> p_client, int p_code, const String &p_json) {
	CharString body = p_json.utf8();
	String reason = p_code == 200 ? "OK" : (p_code == 404 ? "Not Found" : (p_code == 400 ? "Bad Request" : "Error"));
	String head = vformat("HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", p_code, reason, body.length());
	CharString head_cs = head.utf8();
	PackedByteArray out;
	out.resize(head_cs.length() + body.length());
	memcpy(out.ptrw(), head_cs.get_data(), head_cs.length());
	memcpy(out.ptrw() + head_cs.length(), body.get_data(), body.length());
	p_client->put_data(out.ptr(), out.size());
}

bool AIResponsesTranslator::_read_http_request(Ref<StreamPeerTCP> p_client, String &r_method, String &r_path, PackedByteArray &r_body) {
	PackedByteArray buf;
	int header_end = -1;
	int content_length = 0;
	uint64_t last_data_ms = Time::get_singleton()->get_ticks_msec();
	const uint64_t IDLE_TIMEOUT_MS = 20000;

	while (!exit_flag.is_set()) {
		p_client->poll();
		if (p_client->get_status() != StreamPeerTCP::STATUS_CONNECTED) {
			return false;
		}
		int avail = p_client->get_available_bytes();
		if (avail > 0) {
			int old_size = buf.size();
			buf.resize(old_size + avail);
			int received = 0;
			p_client->get_partial_data(buf.ptrw() + old_size, avail, received);
			buf.resize(old_size + MAX(received, 0));
			last_data_ms = Time::get_singleton()->get_ticks_msec();
		} else {
			if (Time::get_singleton()->get_ticks_msec() - last_data_ms > IDLE_TIMEOUT_MS) {
				return false;
			}
			OS::get_singleton()->delay_usec(2000); // 2ms
		}

		if (header_end < 0 && buf.size() >= 4) {
			for (int i = 0; i + 3 < buf.size(); i++) {
				if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
					header_end = i + 4;
					break;
				}
			}
			if (header_end >= 0) {
				String headers = String::utf8((const char *)buf.ptr(), header_end);
				Vector<String> lines = headers.split("\r\n");
				if (lines.size() > 0) {
					Vector<String> req_line = lines[0].split(" ");
					if (req_line.size() >= 2) {
						r_method = req_line[0];
						r_path = req_line[1];
					}
				}
				for (int i = 1; i < lines.size(); i++) {
					if (lines[i].to_lower().begins_with("content-length:")) {
						content_length = lines[i].get_slice(":", 1).strip_edges().to_int();
					}
				}
			}
		}

		if (header_end >= 0 && buf.size() >= header_end + content_length) {
			r_body.resize(content_length);
			if (content_length > 0) {
				memcpy(r_body.ptrw(), buf.ptr() + header_end, content_length);
			}
			return !r_method.is_empty();
		}
	}
	return false;
}

/* -------------------------------------------------------------------- */
/*  Request translation (Responses -> Chat Completions)                  */
/* -------------------------------------------------------------------- */

static String _sanitize_call_id(const String &p_id) {
	// Strict providers reject ':' in tool call ids (and Kimi emits them).
	return p_id.replace(":", "_");
}

// Concatenates the text parts of a Responses content array; collects any
// input_image data URLs separately so vision content survives translation.
static void _split_content(const Variant &p_content, String &r_text, Array &r_image_urls) {
	if (p_content.get_type() == Variant::STRING) {
		r_text += String(p_content);
		return;
	}
	if (p_content.get_type() != Variant::ARRAY) {
		return;
	}
	Array parts = p_content;
	for (int i = 0; i < parts.size(); i++) {
		if (parts[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		Dictionary part = parts[i];
		String type = part.get("type", "");
		if (type == "input_text" || type == "output_text" || type == "text" || type == "summary_text") {
			r_text += String(part.get("text", ""));
		} else if (type == "input_image") {
			String url = part.get("image_url", "");
			if (url.is_empty() && part.has("url")) {
				url = part.get("url", "");
			}
			if (!url.is_empty()) {
				r_image_urls.push_back(url);
			}
		}
	}
}

static bool _is_blank_assistant(const Dictionary &p_msg) {
	if (String(p_msg.get("role", "")) != "assistant" || p_msg.has("tool_calls")) {
		return false;
	}
	Variant content = p_msg.get("content", Variant());
	if (content.get_type() == Variant::STRING) {
		return String(content).strip_edges().is_empty();
	}
	return content.get_type() == Variant::NIL;
}

// Strict providers (Moonshot) require every assistant tool_calls message to be
// IMMEDIATELY followed by its tool responses. Rebuilds the message list to
// guarantee adjacency, synthesizing placeholders for missing results (the same
// no-orphan invariant the engine's own chat store upholds).
static Array _repair_tool_adjacency(const Array &p_messages) {
	Dictionary results_by_id;
	for (int i = 0; i < p_messages.size(); i++) {
		Dictionary m = p_messages[i];
		if (String(m.get("role", "")) == "tool") {
			results_by_id[m.get("tool_call_id", "")] = m;
		}
	}
	Dictionary consumed;
	Array out;
	for (int i = 0; i < p_messages.size(); i++) {
		Dictionary m = p_messages[i];
		String role = m.get("role", "");
		if (role == "tool") {
			continue; // Re-emitted right after its call below.
		}
		out.push_back(m);
		if (role == "assistant" && m.has("tool_calls")) {
			Array calls = m.get("tool_calls", Array());
			for (int c = 0; c < calls.size(); c++) {
				Dictionary call = calls[c];
				String call_id = call.get("id", "");
				if (results_by_id.has(call_id)) {
					out.push_back(results_by_id[call_id]);
					consumed[call_id] = true;
				} else {
					Dictionary synth;
					synth["role"] = "tool";
					synth["tool_call_id"] = call_id;
					synth["content"] = "(no result recorded)";
					out.push_back(synth);
				}
			}
		}
	}
	// Orphan tool results (no matching call in this window) are dropped -
	// forwarding them would trip the same strict validation we're repairing.
	return out;
}

// Codex fragments one model response into separate rollout items (reasoning,
// function_call xN, message) — and its item ordering even puts the text after
// the calls it announced. Replaying those fragments as separate assistant
// messages misrepresents the model's own past behavior: the transcript fills
// with text-only assistant messages mid-work, which teaches the model that
// sampling one live is normal — but a chat-completions API never re-samples
// after a text-only stop, so the turn dies. Accumulate the fragments and
// re-merge them into the single assistant message the model actually produced.
struct PendingAssistant {
	String reasoning;
	String text;
	Array tool_calls;

	bool has_payload() const { return !reasoning.is_empty() || !text.is_empty() || !tool_calls.is_empty(); }
	void reset() {
		reasoning = String();
		text = String();
		tool_calls = Array();
	}
};

static void _flush_pending_assistant(PendingAssistant &p_pending, Array &r_messages) {
	if (p_pending.has_payload()) {
		Dictionary msg;
		msg["role"] = "assistant";
		msg["content"] = p_pending.text;
		if (!p_pending.tool_calls.is_empty()) {
			msg["tool_calls"] = p_pending.tool_calls;
		}
		if (!p_pending.reasoning.is_empty()) {
			msg["reasoning_content"] = p_pending.reasoning;
		}
		r_messages.push_back(msg);
	}
	p_pending.reset();
}

// Reasoning text lives in content[] (reasoning_text/text parts), with
// summary[] (summary_text parts) as fallback — the shapes codex records for
// the reasoning items this translator emits.
static String _extract_reasoning_text(const Dictionary &p_item) {
	String out;
	Array content = p_item.get("content", Array());
	for (int i = 0; i < content.size(); i++) {
		if (content[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		Dictionary part = content[i];
		String type = part.get("type", "");
		if (type == "reasoning_text" || type == "text") {
			String text = part.get("text", "");
			if (!text.is_empty()) {
				if (!out.is_empty()) {
					out += "\n";
				}
				out += text;
			}
		}
	}
	if (!out.is_empty()) {
		return out;
	}
	Array summary = p_item.get("summary", Array());
	for (int i = 0; i < summary.size(); i++) {
		if (summary[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		Dictionary part = summary[i];
		if (String(part.get("type", "")) == "summary_text") {
			String text = part.get("text", "");
			if (!text.is_empty()) {
				if (!out.is_empty()) {
					out += "\n";
				}
				out += text;
			}
		}
	}
	return out;
}

Dictionary AIResponsesTranslator::translate_request(const Dictionary &p_req, String &r_err) {
	Dictionary chat;
	Array messages;

	String instructions = p_req.get("instructions", "");
	if (!instructions.is_empty()) {
		Dictionary sys;
		sys["role"] = "system";
		sys["content"] = instructions;
		messages.push_back(sys);
	}

	Array input = p_req.get("input", Array());

	// Current-turn boundary: replayed reasoning is folded back into assistant
	// messages only for rounds after the last user message (Moonshot's
	// thinking-model guidance covers the in-flight turn; older reasoning is
	// dropped for context economy).
	int last_user_idx = -1;
	for (int i = 0; i < input.size(); i++) {
		if (input[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		Dictionary item = input[i];
		if (String(item.get("type", "message")) == "message" && String(item.get("role", "user")) == "user") {
			last_user_idx = i;
		}
	}

	PendingAssistant pending;
	for (int i = 0; i < input.size(); i++) {
		if (input[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		Dictionary item = input[i];
		String type = item.get("type", "message");

		if (type == "message") {
			String role = item.get("role", "user");
			if (role == "developer" || role == "system") {
				role = "system";
			}
			String text;
			Array image_urls;
			_split_content(item.get("content", Variant()), text, image_urls);
			if (role == "assistant") {
				// Fragment of an assistant round: accumulate, don't emit.
				if (!text.is_empty()) {
					if (!pending.text.is_empty()) {
						pending.text += "\n\n";
					}
					pending.text += text;
				}
				continue;
			}
			_flush_pending_assistant(pending, messages);
			Dictionary msg;
			msg["role"] = role;
			if (image_urls.is_empty()) {
				msg["content"] = text;
			} else {
				// Vision: chat format wants an array of typed parts.
				Array parts;
				if (!text.is_empty()) {
					Dictionary tp;
					tp["type"] = "text";
					tp["text"] = text;
					parts.push_back(tp);
				}
				for (int u = 0; u < image_urls.size(); u++) {
					Dictionary ip;
					ip["type"] = "image_url";
					Dictionary url;
					url["url"] = image_urls[u];
					ip["image_url"] = url;
					parts.push_back(ip);
				}
				msg["content"] = parts;
			}
			messages.push_back(msg);
		} else if (type == "function_call") {
			Dictionary call;
			call["id"] = _sanitize_call_id(item.get("call_id", item.get("id", "")));
			call["type"] = "function";
			Dictionary fn;
			fn["name"] = item.get("name", "");
			Variant args = item.get("arguments", "{}");
			fn["arguments"] = args.get_type() == Variant::STRING ? String(args) : JSON::stringify(args);
			call["function"] = fn;
			pending.tool_calls.push_back(call);
		} else if (type == "function_call_output") {
			_flush_pending_assistant(pending, messages);
			String call_id = _sanitize_call_id(item.get("call_id", ""));
			Dictionary msg;
			msg["role"] = "tool";
			msg["tool_call_id"] = call_id;
			Variant output = item.get("output", "");
			Array image_urls;
			if (output.get_type() == Variant::STRING) {
				msg["content"] = output;
			} else {
				String text;
				_split_content(output, text, image_urls);
				msg["content"] = text.is_empty() ? JSON::stringify(output) : text;
			}
			messages.push_back(msg);
			// Chat providers reject images inside tool messages; hoist them into
			// a follow-up user message so vision content survives (same strategy
			// the LiteLLM bridge used - verified against Kimi in the spike).
			if (!image_urls.is_empty()) {
				Array parts;
				Dictionary tp;
				tp["type"] = "text";
				tp["text"] = vformat("[image returned by tool call %s]", call_id);
				parts.push_back(tp);
				for (int u = 0; u < image_urls.size(); u++) {
					Dictionary ip;
					ip["type"] = "image_url";
					Dictionary url;
					url["url"] = image_urls[u];
					ip["image_url"] = url;
					parts.push_back(ip);
				}
				Dictionary img_msg;
				img_msg["role"] = "user";
				img_msg["content"] = parts;
				messages.push_back(img_msg);
			}
		} else if (type == "reasoning") {
			if (i > last_user_idx) {
				String reasoning_text = _extract_reasoning_text(item);
				if (!reasoning_text.is_empty()) {
					if (!pending.reasoning.is_empty()) {
						pending.reasoning += "\n";
					}
					pending.reasoning += reasoning_text;
				}
			}
		}
		// item_reference / anything else: intentionally skipped — and NOT a
		// flush boundary; fragments on either side still belong to one round.
	}
	_flush_pending_assistant(pending, messages);

	// Transcript hygiene (spike-derived, see harness_replacement_plan.md).
	Array cleaned;
	for (int i = 0; i < messages.size(); i++) {
		if (!_is_blank_assistant(messages[i])) {
			cleaned.push_back(messages[i]);
		}
	}
	chat["messages"] = _repair_tool_adjacency(cleaned);

	Array tools_out;
	Array tools_in = p_req.get("tools", Array());
	for (int i = 0; i < tools_in.size(); i++) {
		if (tools_in[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		Dictionary t = tools_in[i];
		if (String(t.get("type", "")) != "function") {
			continue; // web_search / namespace / built-ins: chat providers can't take them.
		}
		Dictionary fn;
		fn["name"] = t.get("name", "");
		fn["description"] = t.get("description", "");
		fn["parameters"] = t.get("parameters", Dictionary());
		Dictionary tool;
		tool["type"] = "function";
		tool["function"] = fn;
		tools_out.push_back(tool);
	}
	if (!tools_out.is_empty()) {
		chat["tools"] = tools_out;
		if (p_req.get("tool_choice", Variant()).get_type() == Variant::STRING) {
			chat["tool_choice"] = p_req["tool_choice"];
		}
	}

	chat["model"] = p_req.get("model", "");
	if (p_req.has("max_output_tokens")) {
		// Godot's JSON parser yields doubles; Moonshot 400s on "max_tokens": 16.0.
		chat["max_tokens"] = (int64_t)(double)p_req["max_output_tokens"];
	}
	if (p_req.has("temperature")) {
		chat["temperature"] = p_req["temperature"];
	}
	// Kimi K3 rejects any temperature other than 1 with HTTP 400. Codex doesn't
	// send temperature today; clamp defensively in case a future config does.
	if (String(chat["model"]).begins_with("kimi-k3") && chat.has("temperature")) {
		chat["temperature"] = 1;
	}
	if (p_req.has("top_p")) {
		chat["top_p"] = p_req["top_p"];
	}
	// Kimi K3 honors OpenAI-style reasoning_effort and it genuinely scales
	// thinking depth (probed api.moonshot.ai Aug 2026: "low" averaged ~4x fewer
	// reasoning tokens than "high"). The K2-era API rejected the param, which is
	// why it was historically dropped — non-K3 models still get nothing.
	// Forward codex's requested effort, defaulting to high per Aristotle policy.
	if (String(chat["model"]).begins_with("kimi-k3")) {
		String effort = "high";
		if (p_req.get("reasoning", Variant()).get_type() == Variant::DICTIONARY) {
			String requested = Dictionary(p_req["reasoning"]).get("effort", "");
			if (requested == "minimal") {
				requested = "low"; // Moonshot's floor; "minimal" is OpenAI-only.
			}
			if (requested == "low" || requested == "medium" || requested == "high") {
				effort = requested;
			}
		}
		chat["reasoning_effort"] = effort;
	}
	// Dropped deliberately: store, include, prompt_cache_key, client_metadata,
	// parallel_tool_calls, text/format; reasoning.effort for non-K3 models
	// (K2-era Moonshot rejects it).
	chat["stream"] = true;
	Dictionary stream_options;
	stream_options["include_usage"] = true;
	chat["stream_options"] = stream_options;
	// History replayed from codex rollouts can carry control chars (e.g. NUL
	// from a shell command dumping a binary file); strict provider decoders
	// reject them even properly escaped, and the replay makes the failure
	// permanent. Scrub every string value before serialization.
	return ai_sanitize_model_variant(chat);
}

Dictionary AIResponsesTranslator::make_reasoning_done_item(const String &p_id, const String &p_text) {
	Dictionary item;
	item["type"] = "reasoning";
	item["id"] = p_id;
	Dictionary summary_part;
	summary_part["type"] = "summary_text";
	summary_part["text"] = p_text;
	Array summary;
	summary.push_back(summary_part);
	item["summary"] = summary;
	Dictionary content_part;
	content_part["type"] = "reasoning_text";
	content_part["text"] = p_text;
	Array content;
	content.push_back(content_part);
	item["content"] = content;
	// No "status" key: codex's ReasoningResponseItem schema has none.
	return item;
}

/* -------------------------------------------------------------------- */
/*  Connection handling + SSE emitter                                    */
/* -------------------------------------------------------------------- */

void AIResponsesTranslator::_serve_connection(Ref<StreamPeerTCP> p_client) {
	String method, path;
	PackedByteArray body;
	if (!_read_http_request(p_client, method, path, body)) {
		p_client->disconnect_from_host();
		return;
	}

	if (method == "POST" && path.contains("/responses")) {
		String body_str = String::utf8((const char *)body.ptr(), body.size());
		Variant parsed = JSON::parse_string(body_str);
		if (parsed.get_type() != Variant::DICTIONARY) {
			_send_http_json(p_client, 400, "{\"error\":{\"message\":\"Aristotle translator: request body is not valid JSON.\"}}");
		} else {
			_handle_responses(p_client, parsed);
		}
	} else if (method == "GET" && path.contains("/models")) {
		Array data;
		for (const UpstreamEntry &e : UPSTREAMS) {
			Dictionary m;
			m["id"] = e.model;
			m["object"] = "model";
			data.push_back(m);
		}
		Dictionary out;
		out["object"] = "list";
		out["data"] = data;
		_send_http_json(p_client, 200, JSON::stringify(out));
	} else {
		_send_http_json(p_client, 404, "{\"error\":{\"message\":\"Aristotle translator: unknown endpoint.\"}}");
	}
	p_client->disconnect_from_host();
}

// Per-tool-call accumulator for the streaming emitter.
struct ToolAcc {
	String id;
	String name;
	String args;
	String pending_args; // Fragments that arrived before the item was announced.
	int output_index = -1;
	bool announced = false;
};

// Reasoning-phase accumulator: each thinking phase becomes one real reasoning
// output item so codex records it in the rollout and replays it on later
// rounds (where translate_request folds it back into reasoning_content). A
// reopened phase gets a fresh item; the serial suffix keeps ids unique.
struct ReasoningAcc {
	bool open = false;
	int serial = 0;
	String id;
	String text;
	int output_index = -1;
};

void AIResponsesTranslator::_handle_responses(Ref<StreamPeerTCP> p_client, const Dictionary &p_req) {
	String model = p_req.get("model", "");
	String host, upstream_path, api_key, err_msg;
	if (!_resolve_upstream(model, host, upstream_path, api_key, err_msg)) {
		_send_http_json(p_client, 400, JSON::stringify(_make_error(err_msg)));
		return;
	}

	String translate_err;
	Dictionary chat_body = translate_request(p_req, translate_err);
	if (!translate_err.is_empty()) {
		_send_http_json(p_client, 400, JSON::stringify(_make_error(translate_err)));
		return;
	}

	if (!OS::get_singleton()->get_environment("ARISTOTLE_TRANSLATOR_TRACE").is_empty()) {
		// Shape-level trace of the round trip (no content): what codex sent vs
		// what goes upstream. The reasoning replay verification depends on it.
		Array in_items = p_req.get("input", Array());
		String in_desc;
		for (int i = 0; i < in_items.size(); i++) {
			if (in_items[i].get_type() != Variant::DICTIONARY) {
				continue;
			}
			Dictionary in_item = in_items[i];
			String desc = in_item.get("type", "message");
			if (desc == "message") {
				desc += ":" + String(in_item.get("role", "user"));
			}
			if (!in_desc.is_empty()) {
				in_desc += " ";
			}
			in_desc += desc;
		}
		Array out_msgs = chat_body.get("messages", Array());
		String out_desc;
		for (int i = 0; i < out_msgs.size(); i++) {
			if (out_msgs[i].get_type() != Variant::DICTIONARY) {
				continue;
			}
			Dictionary out_msg = out_msgs[i];
			String desc = out_msg.get("role", "");
			if (out_msg.has("tool_calls")) {
				desc += vformat("+tools:%d", Array(out_msg["tool_calls"]).size());
			}
			if (out_msg.has("reasoning_content")) {
				desc += "+reasoning";
			}
			if (!out_desc.is_empty()) {
				out_desc += " ";
			}
			out_desc += desc;
		}
		print_line(vformat("AIResponsesTranslator: input=[%s] -> messages=[%s]", in_desc, out_desc));
	}

	// --- Upstream connection (same poll idiom as the provider layer). ---
	HTTPClient *http = HTTPClient::create();
	Ref<TLSOptions> tls = TLSOptions::client();
	Error err = http->connect_to_host(host, 443, tls);
	if (err != OK) {
		_send_http_json(p_client, 502, JSON::stringify(_make_error(vformat("connect_to_host failed (%d)", err))));
		memdelete(http);
		return;
	}
	while (http->get_status() == HTTPClient::STATUS_CONNECTING || http->get_status() == HTTPClient::STATUS_RESOLVING) {
		if (exit_flag.is_set()) {
			memdelete(http);
			return;
		}
		http->poll();
		OS::get_singleton()->delay_usec(5000);
	}
	if (http->get_status() != HTTPClient::STATUS_CONNECTED) {
		_send_http_json(p_client, 502, JSON::stringify(_make_error(vformat("upstream connection failed (status %d)", http->get_status()))));
		memdelete(http);
		return;
	}

	Vector<String> headers;
	headers.push_back("Content-Type: application/json");
	headers.push_back("Authorization: Bearer " + api_key);
	CharString body_cs = JSON::stringify(chat_body).utf8();
	err = http->request(HTTPClient::METHOD_POST, upstream_path, headers, (const uint8_t *)body_cs.get_data(), body_cs.length());
	if (err != OK) {
		_send_http_json(p_client, 502, JSON::stringify(_make_error(vformat("upstream request failed (%d)", err))));
		memdelete(http);
		return;
	}
	while (http->get_status() == HTTPClient::STATUS_REQUESTING) {
		if (exit_flag.is_set()) {
			memdelete(http);
			return;
		}
		http->poll();
		OS::get_singleton()->delay_usec(5000);
	}
	if (http->get_status() != HTTPClient::STATUS_BODY && http->get_status() != HTTPClient::STATUS_CONNECTED) {
		_send_http_json(p_client, 502, JSON::stringify(_make_error(vformat("upstream transport failed (status %d)", http->get_status()))));
		memdelete(http);
		return;
	}

	int upstream_code = http->get_response_code();
	if (upstream_code != 200) {
		// Forward the provider's error body verbatim - codex surfaces it.
		PackedByteArray err_body;
		while (http->get_status() == HTTPClient::STATUS_BODY) {
			http->poll();
			PackedByteArray chunk = http->read_response_body_chunk();
			if (chunk.size() > 0) {
				err_body.append_array(chunk);
			} else {
				OS::get_singleton()->delay_usec(5000);
			}
		}
		String err_str = String::utf8((const char *)err_body.ptr(), err_body.size());
		_send_http_json(p_client, upstream_code, err_str.is_empty() ? JSON::stringify(_make_error("upstream error")) : err_str);
		memdelete(http);
		return;
	}

	// --- SSE response: header, then events mirroring the golden fixtures. ---
	if (!_send_raw(p_client, String("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n").utf8())) {
		memdelete(http);
		return;
	}

	const String resp_id = vformat("resp_aristotle_%d", (int64_t)Time::get_singleton()->get_ticks_usec());
	int seq = 0;
	Dictionary resp_obj;
	resp_obj["id"] = resp_id;
	resp_obj["object"] = "response";
	resp_obj["created_at"] = (int64_t)Time::get_singleton()->get_unix_time_from_system();
	resp_obj["model"] = model;
	resp_obj["status"] = "in_progress";
	resp_obj["output"] = Array();
	resp_obj["metadata"] = Dictionary();

	auto make_event = [&](const String &p_type) {
		Dictionary ev;
		ev["type"] = p_type;
		ev["sequence_number"] = seq++;
		ev["model"] = model;
		return ev;
	};

	Dictionary ev_created = make_event("response.created");
	ev_created["response"] = resp_obj;
	Dictionary ev_progress = make_event("response.in_progress");
	ev_progress["response"] = resp_obj;
	bool alive = _send_sse_event(p_client, ev_created) && _send_sse_event(p_client, ev_progress);

	// Emitter state.
	bool msg_open = false;
	String msg_id;
	String text_accum;
	int next_output_index = 0;
	int msg_output_index = -1;
	HashMap<int, ToolAcc> tool_accs;
	Dictionary usage;
	String sse_buffer;
	bool done = false;
	String finish_reason;
	ReasoningAcc racc;
	// Kill-switch: restores the pre-reasoning-item behavior (summary deltas on
	// the message item, nothing recorded or replayed) as an instant rollback.
	const bool legacy_reasoning = !OS::get_singleton()->get_environment("ARISTOTLE_TRANSLATOR_LEGACY_REASONING").is_empty();

	auto ensure_msg_open = [&]() {
		if (msg_open) {
			return;
		}
		msg_open = true;
		msg_id = vformat("msg_aristotle_%d", (int64_t)Time::get_singleton()->get_ticks_usec());
		msg_output_index = next_output_index++;
		Dictionary item;
		item["id"] = msg_id;
		item["type"] = "message";
		item["role"] = "assistant";
		item["status"] = "in_progress";
		item["content"] = Array();
		Dictionary ev = make_event("response.output_item.added");
		ev["output_index"] = msg_output_index;
		ev["item"] = item;
		alive = alive && _send_sse_event(p_client, ev);
		Dictionary part;
		part["type"] = "output_text";
		part["text"] = "";
		part["annotations"] = Array();
		Dictionary ev2 = make_event("response.content_part.added");
		ev2["item_id"] = msg_id;
		ev2["output_index"] = msg_output_index;
		ev2["content_index"] = 0;
		ev2["part"] = part;
		alive = alive && _send_sse_event(p_client, ev2);
	};

	auto open_reasoning = [&]() {
		if (racc.open) {
			return;
		}
		racc.open = true;
		racc.text = String();
		racc.id = vformat("rs_aristotle_%d_%d", (int64_t)Time::get_singleton()->get_ticks_usec(), racc.serial++);
		racc.output_index = next_output_index++;
		Dictionary item;
		item["id"] = racc.id;
		item["type"] = "reasoning";
		item["summary"] = Array();
		Dictionary ev = make_event("response.output_item.added");
		ev["output_index"] = racc.output_index;
		ev["item"] = item;
		alive = alive && _send_sse_event(p_client, ev);
	};

	auto close_reasoning = [&]() {
		if (!racc.open) {
			return;
		}
		racc.open = false;
		Dictionary ev = make_event("response.reasoning_summary_text.done");
		ev["item_id"] = racc.id;
		ev["output_index"] = racc.output_index;
		ev["summary_index"] = 0;
		ev["text"] = racc.text;
		alive = alive && _send_sse_event(p_client, ev);
		Dictionary ev2 = make_event("response.output_item.done");
		ev2["output_index"] = racc.output_index;
		ev2["item"] = make_reasoning_done_item(racc.id, racc.text);
		alive = alive && _send_sse_event(p_client, ev2);
	};

	auto process_delta = [&](const Dictionary &p_chunk) {
		if (p_chunk.has("usage") && p_chunk["usage"].get_type() == Variant::DICTIONARY && !Dictionary(p_chunk["usage"]).is_empty()) {
			usage = p_chunk["usage"];
		}
		Array choices = p_chunk.get("choices", Array());
		if (choices.is_empty() || choices[0].get_type() != Variant::DICTIONARY) {
			return;
		}
		Dictionary choice = choices[0];
		Dictionary delta = choice.get("delta", Dictionary());
		Variant fr = choice.get("finish_reason", Variant());
		if (fr.get_type() == Variant::STRING && !String(fr).is_empty()) {
			finish_reason = fr;
		}

		String reasoning = delta.get("reasoning_content", "");
		if (!reasoning.is_empty()) {
			if (legacy_reasoning) {
				ensure_msg_open();
				Dictionary ev = make_event("response.reasoning_summary_text.delta");
				ev["item_id"] = msg_id;
				ev["output_index"] = msg_output_index;
				ev["summary_index"] = 0;
				ev["delta"] = reasoning;
				alive = alive && _send_sse_event(p_client, ev);
			} else {
				open_reasoning();
				racc.text += reasoning;
				Dictionary ev = make_event("response.reasoning_summary_text.delta");
				ev["item_id"] = racc.id;
				ev["output_index"] = racc.output_index;
				ev["summary_index"] = 0;
				ev["delta"] = reasoning;
				alive = alive && _send_sse_event(p_client, ev);
			}
		}
		String content = delta.get("content", "");
		if (!content.is_empty()) {
			close_reasoning();
			ensure_msg_open();
			text_accum += content;
			Dictionary ev = make_event("response.output_text.delta");
			ev["item_id"] = msg_id;
			ev["output_index"] = msg_output_index;
			ev["content_index"] = 0;
			ev["delta"] = content;
			alive = alive && _send_sse_event(p_client, ev);
		}
		Array tool_calls = delta.get("tool_calls", Array());
		if (!tool_calls.is_empty()) {
			close_reasoning();
		}
		for (int t = 0; t < tool_calls.size(); t++) {
			if (tool_calls[t].get_type() != Variant::DICTIONARY) {
				continue;
			}
			Dictionary tc = tool_calls[t];
			int idx = tc.get("index", 0);
			ToolAcc &acc = tool_accs[idx];
			String id = tc.get("id", "");
			if (!id.is_empty()) {
				acc.id = _sanitize_call_id(id);
			}
			Dictionary fn = tc.get("function", Dictionary());
			String name = fn.get("name", "");
			if (!name.is_empty()) {
				acc.name = name;
			}
			if (!acc.announced && !acc.name.is_empty()) {
				if (acc.id.is_empty()) {
					acc.id = vformat("%s_%d", acc.name, idx);
				}
				acc.announced = true;
				acc.output_index = next_output_index++;
				Dictionary item;
				item["type"] = "function_call";
				item["id"] = acc.id;
				item["call_id"] = acc.id;
				item["name"] = acc.name;
				item["status"] = "in_progress";
				item["arguments"] = "";
				Dictionary ev = make_event("response.output_item.added");
				ev["output_index"] = acc.output_index;
				ev["item"] = item;
				alive = alive && _send_sse_event(p_client, ev);
				if (!acc.pending_args.is_empty()) {
					Dictionary ev2 = make_event("response.function_call_arguments.delta");
					ev2["item_id"] = acc.id;
					ev2["output_index"] = acc.output_index;
					ev2["delta"] = acc.pending_args;
					alive = alive && _send_sse_event(p_client, ev2);
					acc.args += acc.pending_args;
					acc.pending_args = String();
				}
			}
			String arg_frag = fn.get("arguments", "");
			if (!arg_frag.is_empty()) {
				if (!acc.announced) {
					acc.pending_args += arg_frag;
				} else {
					acc.args += arg_frag;
					Dictionary ev = make_event("response.function_call_arguments.delta");
					ev["item_id"] = acc.id;
					ev["output_index"] = acc.output_index;
					ev["delta"] = arg_frag;
					alive = alive && _send_sse_event(p_client, ev);
				}
			}
		}
	};

	// --- Stream loop: read upstream chunks, split SSE lines, emit. ---
	while (!done && alive && !exit_flag.is_set() && http->get_status() == HTTPClient::STATUS_BODY) {
		http->poll();
		PackedByteArray chunk = http->read_response_body_chunk();
		if (chunk.size() == 0) {
			if (!_client_alive(p_client)) {
				break; // codex aborted (interrupt/steer) - drop upstream.
			}
			OS::get_singleton()->delay_usec(4000);
			continue;
		}
		sse_buffer += String::utf8((const char *)chunk.ptr(), chunk.size());
		int nl;
		while ((nl = sse_buffer.find("\n")) >= 0) {
			String line = sse_buffer.substr(0, nl).strip_edges();
			sse_buffer = sse_buffer.substr(nl + 1);
			if (!line.begins_with("data:")) {
				continue;
			}
			String payload = line.substr(5).strip_edges();
			if (payload == "[DONE]") {
				done = true;
				break;
			}
			Variant parsed = JSON::parse_string(payload);
			if (parsed.get_type() == Variant::DICTIONARY) {
				process_delta(parsed);
			}
		}
	}
	memdelete(http);

	if (!alive) {
		p_client->disconnect_from_host();
		return;
	}

	// --- Closing events (order per golden fixtures; reasoning first so the
	// rollout records each round as [reasoning, function_call..., message],
	// all assistant-side-adjacent for translate_request's merge). ---
	close_reasoning();
	for (const KeyValue<int, ToolAcc> &kv : tool_accs) {
		const ToolAcc &acc = kv.value;
		if (!acc.announced) {
			continue;
		}
		Dictionary ev = make_event("response.function_call_arguments.done");
		ev["item_id"] = acc.id;
		ev["output_index"] = acc.output_index;
		ev["arguments"] = acc.args.is_empty() ? String("{}") : acc.args;
		_send_sse_event(p_client, ev);
		Dictionary item;
		item["type"] = "function_call";
		item["id"] = acc.id;
		item["call_id"] = acc.id;
		item["name"] = acc.name;
		item["status"] = "completed";
		item["arguments"] = acc.args.is_empty() ? String("{}") : acc.args;
		Dictionary ev2 = make_event("response.output_item.done");
		ev2["output_index"] = acc.output_index;
		ev2["item"] = item;
		_send_sse_event(p_client, ev2);
	}
	if (msg_open) {
		Dictionary ev = make_event("response.output_text.done");
		ev["item_id"] = msg_id;
		ev["output_index"] = msg_output_index;
		ev["content_index"] = 0;
		ev["text"] = text_accum;
		_send_sse_event(p_client, ev);
		Dictionary part;
		part["type"] = "output_text";
		part["text"] = text_accum;
		part["annotations"] = Array();
		Dictionary ev2 = make_event("response.content_part.done");
		ev2["item_id"] = msg_id;
		ev2["output_index"] = msg_output_index;
		ev2["content_index"] = 0;
		ev2["part"] = part;
		_send_sse_event(p_client, ev2);
		Dictionary content_entry;
		content_entry["type"] = "output_text";
		content_entry["text"] = text_accum;
		content_entry["annotations"] = Array();
		Array content;
		content.push_back(content_entry);
		Dictionary item;
		item["id"] = msg_id;
		item["type"] = "message";
		item["role"] = "assistant";
		item["status"] = "completed";
		item["content"] = content;
		Dictionary ev3 = make_event("response.output_item.done");
		ev3["output_index"] = msg_output_index;
		ev3["item"] = item;
		_send_sse_event(p_client, ev3);
	}

	Dictionary final_usage;
	// Upstream JSON numbers arrive as Variant floats; codex's typed structs
	// require integers here ("33.0" fails deserialization and the completed
	// event is silently dropped, failing the whole turn).
	final_usage["input_tokens"] = (int64_t)(double)usage.get("prompt_tokens", 0);
	final_usage["output_tokens"] = (int64_t)(double)usage.get("completion_tokens", 0);
	Dictionary details;
	Dictionary completion_details = usage.get("completion_tokens_details", Dictionary());
	details["reasoning_tokens"] = (int64_t)(double)completion_details.get("reasoning_tokens", 0);
	final_usage["output_tokens_details"] = details;
	final_usage["total_tokens"] = (int64_t)(double)usage.get("total_tokens", 0);
	resp_obj["usage"] = final_usage;
	if (finish_reason == "length") {
		// The provider cut generation at the max_tokens budget (which Kimi's
		// thinking shares). Without this branch the truncation is served as a
		// clean completion — indistinguishable from a deliberate stop, and if
		// the cut landed before a tool call, the turn silently dies.
		resp_obj["status"] = "failed";
		Dictionary error;
		error["code"] = "output_limit";
		error["message"] = (msg_open || !tool_accs.is_empty())
				? String("Kimi hit the output token limit mid-response (finish_reason=length); the reply was truncated.")
				: String("Kimi hit the output token limit while reasoning (finish_reason=length); no visible output was produced.");
		resp_obj["error"] = error;
		Dictionary ev_failed = make_event("response.failed");
		ev_failed["response"] = resp_obj;
		_send_sse_event(p_client, ev_failed);
		_send_raw(p_client, String("data: [DONE]\n\n").utf8());
		p_client->disconnect_from_host();
		return;
	}
	resp_obj["status"] = "completed";
	Dictionary ev_done = make_event("response.completed");
	ev_done["response"] = resp_obj;
	_send_sse_event(p_client, ev_done);
	_send_raw(p_client, String("data: [DONE]\n\n").utf8());
	p_client->disconnect_from_host();
}
