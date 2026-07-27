/**************************************************************************/
/*  responses_translator.h                                                */
/**************************************************************************/
/* Native replacement for the LiteLLM sidecar used during the harness     */
/* spike: a localhost HTTP listener that accepts OpenAI Responses-API     */
/* requests (the only wire format codex app-server speaks) and forwards   */
/* them to Chat Completions providers (Moonshot/Kimi), translating the    */
/* streamed reply back into Responses SSE events.                         */
/*                                                                        */
/* Runs entirely on its own threads; touches no editor state, so it has   */
/* no main-thread constraints. Spec: golden SSE fixtures captured from    */
/* LiteLLM in modules/ai/harness_spike/fixtures/ (which codex accepted),  */
/* plus the provider-compat rules discovered in the Phase 0 spike:        */
/* param dropping, blank-assistant stripping, tool-call adjacency repair, */
/* non-function tool filtering. See docs/design/harness_replacement_plan. */
/**************************************************************************/

#pragma once

#include "core/object/ref_counted.h"
#include "core/os/thread.h"
#include "core/templates/safe_refcount.h"

class TCPServer;
class StreamPeerTCP;

class AIResponsesTranslator {
public:
	static AIResponsesTranslator *get_singleton();

	// Starts listening on 127.0.0.1:p_port. Idempotent; returns false on bind failure.
	bool start(int p_port);
	void stop();
	bool is_running() const { return running.is_set(); }
	int get_port() const { return port; }

private:
	static AIResponsesTranslator *singleton;

	enum { MAX_CONNECTIONS = 4 };

	struct ConnSlot {
		Thread thread;
		SafeFlag busy;
		SafeFlag started;
	};

	struct ConnContext {
		AIResponsesTranslator *translator = nullptr;
		Ref<StreamPeerTCP> peer;
		int slot = -1;
	};

	Ref<TCPServer> server;
	Thread accept_thread;
	SafeFlag running;
	SafeFlag exit_flag;
	int port = 0;
	ConnSlot conn_slots[MAX_CONNECTIONS];

	static void _accept_thread_func(void *p_userdata);
	static void _conn_thread_func(void *p_userdata);

	void _accept_loop();
	void _serve_connection(Ref<StreamPeerTCP> p_client);

	// HTTP plumbing (server side; one request per connection, Connection: close).
	bool _read_http_request(Ref<StreamPeerTCP> p_client, String &r_method, String &r_path, PackedByteArray &r_body);
	void _send_http_json(Ref<StreamPeerTCP> p_client, int p_code, const String &p_json);
	bool _send_raw(Ref<StreamPeerTCP> p_client, const CharString &p_data);
	bool _send_sse_event(Ref<StreamPeerTCP> p_client, const Dictionary &p_event);
	bool _client_alive(Ref<StreamPeerTCP> p_client);

	// Responses -> Chat Completions translation.
	Dictionary _translate_request(const Dictionary &p_req, String &r_err);
	void _handle_responses(Ref<StreamPeerTCP> p_client, const Dictionary &p_req);

	// Upstream registry + secrets.
	bool _resolve_upstream(const String &p_model, String &r_host, String &r_path, String &r_api_key, String &r_err);
	static String _load_key(const String &p_env_var);
};
