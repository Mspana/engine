/**************************************************************************/
/*  ai_remote_server.h                                                    */
/**************************************************************************/
/* Remote access listener: serves the embedded web client and bridges     */
/* paired devices to AIChatSession over an end-to-end encrypted channel.  */
/*                                                                        */
/* Off by default. Loopback mode is plain HTTP (localhost is already a    */
/* secure context, so the browser's WebCrypto works). LAN mode requires   */
/* TLS with a generated self-signed certificate, and every accepted       */
/* socket is checked against private address ranges — a misconfigured     */
/* router still cannot expose this to the internet.                       */
/*                                                                        */
/* THREADING: an accept thread plus one thread per connection slot. Only  */
/* main-thread code touches AIChatSession; connection threads marshal     */
/* through call_deferred and exchange bytes through mutex-guarded queues. */
/**************************************************************************/

#ifndef AI_REMOTE_SERVER_H
#define AI_REMOTE_SERVER_H

#include "ai_remote_devices.h"
#include "ai_remote_protocol.h"
#include "ai_remote_ws.h"

#include "core/crypto/crypto.h"
#include "core/io/stream_peer_tcp.h"
#include "core/io/stream_peer_tls.h"
#include "core/io/tcp_server.h"
#include "core/object/object.h"
#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/templates/hash_map.h"
#include "core/templates/list.h"
#include "core/templates/safe_refcount.h"

class AIRemoteServer : public Object {
	GDCLASS(AIRemoteServer, Object);

	static AIRemoteServer *singleton;

public:
	enum BindMode {
		BIND_LOOPBACK = 0, // 127.0.0.1, no TLS
		BIND_LAN = 1, // 0.0.0.0 with TLS, private peers only
	};

	static const int DEFAULT_PORT = 8420;
	static const int MAX_CONNECTIONS = 4;
	static const int PAIRING_WINDOW_MS = 120 * 1000;
	static const int PAIRING_MAX_ATTEMPTS = 5;
	static const int SESSION_IDLE_MS = 30 * 60 * 1000;
	static const int SESSION_ABSOLUTE_MS = 12 * 60 * 60 * 1000;
	static const int HANDSHAKE_TIMEOUT_MS = 15 * 1000;
	static const int FAILED_ATTEMPTS_PER_IP = 5;
	static const int FAILED_WINDOW_MS = 60 * 1000;

private:
	enum ConnPhase {
		CONN_FREE,
		CONN_HTTP, // reading the request, may become a file response or an upgrade
		CONN_WS_HANDSHAKING,
		CONN_WS_OPEN,
		CONN_CLOSING,
	};

	struct Conn {
		Thread thread;
		SafeFlag in_use;
		SafeFlag stop;

		Ref<StreamPeerTCP> tcp;
		Ref<StreamPeerTLS> tls;
		Ref<StreamPeer> stream;

		int index = 0;
		ConnPhase phase = CONN_FREE;
		String peer_desc;
		// Relay mode dials out, so this socket plays the WebSocket *client*
		// role: outbound frames must be masked, inbound ones must not be.
		bool client_role = false;

		AIRemoteWS::MessageAssembler assembler;
		AIRemoteChannel channel;
		PackedByteArray rx;

		// Handshake scratch (connection thread only).
		PackedByteArray device_public;
		PackedByteArray ephemeral_private;
		PackedByteArray ephemeral_public;
		PackedByteArray nonce_client;
		PackedByteArray nonce_server;
		bool authenticated = false;

		String device_id;
		String device_name;
		bool can_control = false;

		uint64_t connected_at_ms = 0;
		uint64_t authed_at_ms = 0;
		uint64_t last_activity_ms = 0;

		Mutex outbox_mutex;
		List<PackedByteArray> outbox; // plaintext JSON, encrypted on the way out
	};

	Ref<TCPServer> server;
	Thread accept_thread;
	SafeFlag accept_exit;
	SafeFlag running;

	Conn conns[MAX_CONNECTIONS];

	BindMode bind_mode = BIND_LOOPBACK;
	int port = DEFAULT_PORT;
	String preferred_address; // user override; empty means "use the ranking"
	bool use_tls = false;
	Ref<TLSOptions> tls_options;

	AIRemoteDeviceStore devices;
	Mutex devices_mutex; // devices is read from connection threads

	// Pairing window (main thread writes, connection threads read under mutex).
	Mutex pairing_mutex;
	PackedByteArray pairing_secret;
	uint64_t pairing_expires_ms = 0;
	int pairing_attempts = 0;

	// Per-IP failed handshake throttle.
	struct FailRecord {
		int count = 0;
		uint64_t window_start_ms = 0;
	};
	Mutex fail_mutex;
	HashMap<String, FailRecord> fail_counts;

	bool session_connected = false;

	// Relay mode: one outbound connection instead of a listener.
	bool relay_mode = false;
	String relay_url;
	Thread relay_thread;
	SafeFlag relay_exit;

	static void _accept_thread_func(void *p_userdata);
	static void _conn_thread_func(void *p_userdata);
	static void _relay_thread_func(void *p_userdata);
	void _accept_loop();
	void _conn_loop(Conn *p_conn);
	void _relay_loop();
	// Shared message pump; both transports run this once the socket is open.
	void _ws_pump(Conn *p_conn);
	// Masks when acting as a WebSocket client, per RFC6455.
	void _send_ws(Conn *p_conn, int p_opcode, const PackedByteArray &p_payload);
	bool _relay_connect(Conn *p_conn, const String &p_url);
	// Relay control frames ("relay_hello", "peer_here", "peer_gone") are the
	// transport talking, not the peer. Returns true when consumed.
	bool _handle_relay_control(Conn *p_conn, const Dictionary &p_msg);

	bool _peer_allowed(const IPAddress &p_ip) const;
	bool _note_failure(const String &p_ip); // false when the IP is throttled
	bool _is_throttled(const String &p_ip);

	// HTTP + WebSocket bring-up (connection thread).
	bool _read_http_request(Conn *p_conn, String &r_method, String &r_path, HashMap<String, String> &r_headers);
	void _serve_file(Conn *p_conn, const String &p_path);
	void _send_http(Conn *p_conn, int p_code, const String &p_status, const String &p_mime,
			const uint8_t *p_body, int p_len);
	bool _do_ws_handshake(Conn *p_conn, const HashMap<String, String> &p_headers);

	// Protocol (connection thread).
	void _handle_ws_message(Conn *p_conn, const PackedByteArray &p_payload, int p_opcode);
	void _handle_plain_json(Conn *p_conn, const Dictionary &p_msg);
	void _handle_encrypted_json(Conn *p_conn, const Dictionary &p_msg);
	void _send_plain_json(Conn *p_conn, const Dictionary &p_msg);
	void _queue_json(Conn *p_conn, const Dictionary &p_msg);
	void _flush_outbox(Conn *p_conn);
	void _close_conn(Conn *p_conn, int p_code, const String &p_reason);

	// Main-thread bridge.
	void _dispatch_command(int p_conn_index, const Dictionary &p_msg);
	void _connect_session();
	void _disconnect_session();
	void _broadcast(const Dictionary &p_msg, bool p_only_controllers = false);
	Dictionary _state_dict(bool p_can_control) const;

	void _on_session_item_appended(int64_t p_ts, const Dictionary &p_data);
	void _on_session_history_changed(const String &p_reason);
	void _on_session_chat_switched(const String &p_chat_id);
	void _on_session_delta(const String &p_kind, const String &p_text);
	void _on_session_run_state(bool p_running);
	void _on_session_status(const String &p_text);
	void _on_session_approval(const Dictionary &p_info);
	void _on_session_policy_mode(int p_mode);

	bool _ensure_tls();
	static Dictionary _sanitize_item(const Dictionary &p_item);

protected:
	static void _bind_methods();

public:
	static AIRemoteServer *get_singleton() { return singleton; }

	Error start(int p_bind_mode, int p_port);
	// Outbound relay transport: dials p_url and holds the connection open. No
	// port is opened locally. The relay only ever sees ciphertext.
	Error start_relay(const String &p_url);
	void stop();
	bool is_relay_mode() const { return relay_mode; }
	bool is_running() const { return running.is_set(); }
	int get_port() const { return port; }
	int get_bind_mode() const { return (int)bind_mode; }
	// URL to open on the client device, e.g. https://192.168.1.20:8420
	String get_client_url() const;
	int get_active_connection_count() const;

	// ---- LAN address selection ----------------------------------------------
	// Candidate addresses a phone could actually reach, best first:
	// [{address, interface, recommended}, ...]. VPN and virtual adapters are
	// ranked below real LAN interfaces rather than hidden, because the guess
	// can be wrong and the user may need to override it.
	static Array list_lan_addresses();
	// Ranks one candidate. Higher is better; negative means "not reachable as a
	// LAN address". Public and pure so it can be unit-tested directly.
	static int score_lan_candidate(const String &p_address, const String &p_interface_name);
	void set_preferred_address(const String &p_address);
	String get_preferred_address() const { return preferred_address; }
	// The address get_client_url() will actually use.
	String resolve_lan_address() const;

	// ---- Pairing -------------------------------------------------------------
	// Opens a single-use pairing window and returns the code to type into the
	// client. Empty on failure.
	String begin_pairing();
	void cancel_pairing();
	bool is_pairing_active();
	int get_pairing_seconds_left();

	// ---- Devices -------------------------------------------------------------
	Array get_devices();
	bool revoke_device(const String &p_id);
	bool revoke_all_devices();
	bool set_device_control(const String &p_id, bool p_allow);

	AIRemoteServer();
	~AIRemoteServer();
};

#endif // AI_REMOTE_SERVER_H
