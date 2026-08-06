/**************************************************************************/
/*  ai_remote_server.cpp                                                  */
/**************************************************************************/

#include "ai_remote_server.h"

#include "../session/ai_chat_session.h"
#include "ai_remote_crypto.h"
#include "ai_remote_webclient.gen.h"

#include "core/crypto/crypto.h"
#include "core/io/file_access.h"
#include "core/io/ip.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/variant/callable.h"

#include <string.h>

AIRemoteServer *AIRemoteServer::singleton = nullptr;

static uint64_t _now_ms() {
	return OS::get_singleton()->get_ticks_msec();
}

AIRemoteServer::AIRemoteServer() {
	singleton = this;
	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		conns[i].index = i;
	}
}

AIRemoteServer::~AIRemoteServer() {
	stop();
	if (singleton == this) {
		singleton = nullptr;
	}
}

void AIRemoteServer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start", "bind_mode", "port"), &AIRemoteServer::start);
	ClassDB::bind_method(D_METHOD("stop"), &AIRemoteServer::stop);
	ClassDB::bind_method(D_METHOD("is_running"), &AIRemoteServer::is_running);
	ClassDB::bind_method(D_METHOD("get_client_url"), &AIRemoteServer::get_client_url);
	ClassDB::bind_method(D_METHOD("begin_pairing"), &AIRemoteServer::begin_pairing);
	ClassDB::bind_method(D_METHOD("cancel_pairing"), &AIRemoteServer::cancel_pairing);
	ClassDB::bind_method(D_METHOD("is_pairing_active"), &AIRemoteServer::is_pairing_active);
	ClassDB::bind_method(D_METHOD("get_pairing_seconds_left"), &AIRemoteServer::get_pairing_seconds_left);
	ClassDB::bind_method(D_METHOD("get_devices"), &AIRemoteServer::get_devices);
	ClassDB::bind_method(D_METHOD("revoke_device", "id"), &AIRemoteServer::revoke_device);
	ClassDB::bind_method(D_METHOD("revoke_all_devices"), &AIRemoteServer::revoke_all_devices);
	ClassDB::bind_method(D_METHOD("set_device_control", "id", "allow"), &AIRemoteServer::set_device_control);
	ClassDB::bind_method(D_METHOD("get_active_connection_count"), &AIRemoteServer::get_active_connection_count);
	ClassDB::bind_static_method("AIRemoteServer", D_METHOD("list_lan_addresses"), &AIRemoteServer::list_lan_addresses);
	ClassDB::bind_method(D_METHOD("set_preferred_address", "address"), &AIRemoteServer::set_preferred_address);
	ClassDB::bind_method(D_METHOD("get_preferred_address"), &AIRemoteServer::get_preferred_address);
	ClassDB::bind_method(D_METHOD("resolve_lan_address"), &AIRemoteServer::resolve_lan_address);
	ClassDB::bind_method(D_METHOD("_dispatch_command", "conn_index", "msg"), &AIRemoteServer::_dispatch_command);

	ADD_SIGNAL(MethodInfo("state_changed"));
	ADD_SIGNAL(MethodInfo("devices_changed"));
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Error AIRemoteServer::start(int p_bind_mode, int p_port) {
	if (running.is_set()) {
		return ERR_ALREADY_IN_USE;
	}
	ERR_FAIL_COND_V(p_port < 1024 || p_port > 65535, ERR_INVALID_PARAMETER);

	bind_mode = (p_bind_mode == BIND_LAN) ? BIND_LAN : BIND_LOOPBACK;
	port = p_port;
	use_tls = (bind_mode == BIND_LAN);

	{
		MutexLock lock(devices_mutex);
		devices.load();
		if (!devices.ensure_identity()) {
			ERR_PRINT("AIRemoteServer: could not establish a server identity.");
			return FAILED;
		}
	}

	if (use_tls && !_ensure_tls()) {
		ERR_PRINT("AIRemoteServer: TLS setup failed; refusing to start LAN mode without it.");
		return FAILED;
	}

	server.instantiate();
	IPAddress bind_ip = (bind_mode == BIND_LAN) ? IPAddress("0.0.0.0") : IPAddress("127.0.0.1");
	Error err = server->listen(port, bind_ip);
	if (err != OK) {
		ERR_PRINT(vformat("AIRemoteServer: cannot listen on port %d (error %d).", port, (int)err));
		server.unref();
		return err;
	}

	running.set();
	accept_exit.clear();
	accept_thread.start(_accept_thread_func, this);

	_connect_session();

	Dictionary det;
	det["mode"] = bind_mode == BIND_LAN ? "lan" : "loopback";
	det["port"] = port;
	AIRemoteDeviceStore::audit("server_started", det);
	print_line(vformat("AI Remote: listening on %s", get_client_url()));
	emit_signal(SNAME("state_changed"));
	return OK;
}

void AIRemoteServer::stop() {
	if (!running.is_set()) {
		return;
	}
	running.clear();
	accept_exit.set();
	relay_exit.set();
	if (relay_thread.is_started()) {
		relay_thread.wait_to_finish();
	}

	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		conns[i].stop.set();
	}
	if (accept_thread.is_started()) {
		accept_thread.wait_to_finish();
	}
	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		if (conns[i].thread.is_started()) {
			conns[i].thread.wait_to_finish();
		}
		conns[i].in_use.clear();
		conns[i].stop.clear();
		conns[i].stream.unref();
		conns[i].tls.unref();
		conns[i].tcp.unref();
		conns[i].channel.reset();
		conns[i].phase = CONN_FREE;
	}

	if (server.is_valid()) {
		server->stop();
		server.unref();
	}
	cancel_pairing();
	_disconnect_session();
	relay_mode = false;
	relay_url = String();

	AIRemoteDeviceStore::audit("server_stopped", Dictionary());
	print_line("AI Remote: stopped.");
	emit_signal(SNAME("state_changed"));
}

// Adapter names that usually belong to a VPN, container bridge or hypervisor.
// Matching one does not disqualify an address — it just ranks below a real LAN
// interface, because these are frequently unreachable from a phone.
static bool _looks_virtual(const String &p_name) {
	const String n = p_name.to_lower();
	static const char *markers[] = {
		"nordlynx", "openvpn", "wireguard", "tailscale", "zerotier", "vpn",
		"vethernet", "hyper-v", "virtual", "vmware", "virtualbox", "wsl",
		"docker", "tap-", "tun", "loopback", "bluetooth", "teredo", nullptr
	};
	for (int i = 0; markers[i]; i++) {
		if (n.contains(markers[i])) {
			return true;
		}
	}
	return false;
}

int AIRemoteServer::score_lan_candidate(const String &p_address, const String &p_interface_name) {
	if (!p_address.is_valid_ip_address()) {
		return -1;
	}
	// IPv6 is not offered: a phone URL needs bracket syntax and these are
	// almost always link-local anyway.
	const Vector<String> parts = p_address.split(".");
	if (parts.size() != 4) {
		return -1;
	}
	int o[4];
	for (int i = 0; i < 4; i++) {
		if (!parts[i].is_valid_int()) {
			return -1;
		}
		o[i] = parts[i].to_int();
		if (o[i] < 0 || o[i] > 255) {
			return -1;
		}
	}

	if (o[0] == 127) {
		return -1; // loopback
	}
	if (o[0] == 169 && o[1] == 254) {
		return -1; // link-local: an unconfigured adapter, never routable
	}

	int score;
	if (o[0] == 192 && o[1] == 168) {
		score = 300; // overwhelmingly the common home LAN
	} else if (o[0] == 172 && o[1] >= 16 && o[1] <= 31) {
		score = 200;
	} else if (o[0] == 10) {
		score = 100; // also where most VPNs live
	} else {
		return -1; // not a private range
	}

	if (_looks_virtual(p_interface_name)) {
		score -= 250;
	}
	return score;
}

struct AILanCandidate {
	String address;
	String interface_name;
	int score = 0;
};

struct AILanCandidateSort {
	bool operator()(const AILanCandidate &a, const AILanCandidate &b) const {
		if (a.score != b.score) {
			return a.score > b.score;
		}
		return a.address < b.address; // stable, and deterministic across runs
	}
};

Array AIRemoteServer::list_lan_addresses() {
	Vector<AILanCandidate> found;

	HashMap<String, IP::Interface_Info> interfaces;
	IP::get_singleton()->get_local_interfaces(&interfaces);
	for (const KeyValue<String, IP::Interface_Info> &entry : interfaces) {
		const String label = entry.value.name_friendly.is_empty()
				? entry.value.name
				: entry.value.name_friendly;
		for (const IPAddress &ip : entry.value.ip_addresses) {
			if (!ip.is_valid() || !ip.is_ipv4()) {
				continue;
			}
			const String text = String(ip);
			const int score = score_lan_candidate(text, label);
			if (score < 0) {
				continue;
			}
			AILanCandidate c;
			c.address = text;
			c.interface_name = label;
			c.score = score;
			found.push_back(c);
		}
	}

	found.sort_custom<AILanCandidateSort>();

	Array out;
	for (int i = 0; i < found.size(); i++) {
		Dictionary d;
		d["address"] = found[i].address;
		d["interface"] = found[i].interface_name;
		d["recommended"] = (i == 0);
		out.push_back(d);
	}
	return out;
}

String AIRemoteServer::resolve_lan_address() const {
	const Array candidates = list_lan_addresses();
	// An explicit choice wins, but only while that address still exists —
	// docking a laptop or dropping a VPN changes what is available.
	if (!preferred_address.is_empty()) {
		for (int i = 0; i < candidates.size(); i++) {
			if (String(Dictionary(candidates[i]).get("address", "")) == preferred_address) {
				return preferred_address;
			}
		}
	}
	if (!candidates.is_empty()) {
		return Dictionary(candidates[0]).get("address", "");
	}
	return String();
}

void AIRemoteServer::set_preferred_address(const String &p_address) {
	preferred_address = p_address;
	emit_signal(SNAME("state_changed"));
}

String AIRemoteServer::get_client_url() const {
	const String scheme = use_tls ? "https" : "http";
	if (bind_mode == BIND_LOOPBACK) {
		return vformat("%s://localhost:%d", scheme, port);
	}
	const String address = resolve_lan_address();
	if (address.is_empty()) {
		return vformat("%s://<no local network address>:%d", scheme, port);
	}
	return vformat("%s://%s:%d", scheme, address, port);
}

int AIRemoteServer::get_active_connection_count() const {
	int n = 0;
	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		if (conns[i].in_use.is_set()) {
			n++;
		}
	}
	return n;
}

bool AIRemoteServer::_ensure_tls() {
	const String key_path = "user://ai_remote/key.pem";
	const String cert_path = "user://ai_remote/cert.pem";

	Ref<CryptoKey> key;
	Ref<X509Certificate> cert;

	if (FileAccess::exists(key_path) && FileAccess::exists(cert_path)) {
		key = Ref<CryptoKey>(CryptoKey::create());
		cert = Ref<X509Certificate>(X509Certificate::create());
		if (key.is_valid() && cert.is_valid() &&
				key->load(key_path) == OK && cert->load(cert_path) == OK) {
			tls_options = TLSOptions::server(key, cert);
			return tls_options.is_valid();
		}
		WARN_PRINT("AIRemoteServer: stored certificate unusable; regenerating.");
		key.unref();
		cert.unref();
	}

	Ref<Crypto> crypto = Ref<Crypto>(Crypto::create());
	if (crypto.is_null()) {
		return false;
	}
	// RSA rather than EC: mbedTLS' self-signed generator here is RSA-based, and
	// browsers accept it fine once the certificate is trusted manually.
	key = crypto->generate_rsa(2048);
	if (key.is_null()) {
		return false;
	}
	cert = crypto->generate_self_signed_certificate(key,
			"CN=aristotle-remote,O=Aristotle,C=US", "20240101000000", "20440101000000");
	if (cert.is_null()) {
		return false;
	}
	key->save(key_path);
	cert->save(cert_path);

	tls_options = TLSOptions::server(key, cert);
	AIRemoteDeviceStore::audit("tls_certificate_generated", Dictionary());
	return tls_options.is_valid();
}

// ---------------------------------------------------------------------------
// Peer filtering + throttling
// ---------------------------------------------------------------------------

bool AIRemoteServer::_peer_allowed(const IPAddress &p_ip) const {
	if (!p_ip.is_valid()) {
		return false;
	}
	if (p_ip.is_ipv4()) {
		const uint8_t *o = p_ip.get_ipv4();
		if (o[0] == 127) {
			return true; // loopback
		}
		if (bind_mode == BIND_LOOPBACK) {
			return false;
		}
		if (o[0] == 10) {
			return true;
		}
		if (o[0] == 172 && o[1] >= 16 && o[1] <= 31) {
			return true;
		}
		if (o[0] == 192 && o[1] == 168) {
			return true;
		}
		if (o[0] == 169 && o[1] == 254) {
			return true; // link-local
		}
		return false;
	}

	const uint8_t *o = p_ip.get_ipv6();
	bool is_loopback = true;
	for (int i = 0; i < 15; i++) {
		if (o[i] != 0) {
			is_loopback = false;
			break;
		}
	}
	if (is_loopback && o[15] == 1) {
		return true;
	}
	if (bind_mode == BIND_LOOPBACK) {
		return false;
	}
	if ((o[0] & 0xFE) == 0xFC) {
		return true; // unique local fc00::/7
	}
	if (o[0] == 0xFE && (o[1] & 0xC0) == 0x80) {
		return true; // link-local fe80::/10
	}
	return false;
}

bool AIRemoteServer::_is_throttled(const String &p_ip) {
	MutexLock lock(fail_mutex);
	FailRecord *rec = fail_counts.getptr(p_ip);
	if (!rec) {
		return false;
	}
	const uint64_t now = _now_ms();
	if (now - rec->window_start_ms > (uint64_t)FAILED_WINDOW_MS) {
		rec->count = 0;
		rec->window_start_ms = now;
		return false;
	}
	return rec->count >= FAILED_ATTEMPTS_PER_IP;
}

bool AIRemoteServer::_note_failure(const String &p_ip) {
	MutexLock lock(fail_mutex);
	const uint64_t now = _now_ms();
	FailRecord *rec = fail_counts.getptr(p_ip);
	if (!rec) {
		FailRecord fresh;
		fresh.count = 1;
		fresh.window_start_ms = now;
		fail_counts.insert(p_ip, fresh);
		return true;
	}
	if (now - rec->window_start_ms > (uint64_t)FAILED_WINDOW_MS) {
		rec->count = 1;
		rec->window_start_ms = now;
		return true;
	}
	rec->count++;
	return rec->count < FAILED_ATTEMPTS_PER_IP;
}

// ---------------------------------------------------------------------------
// Accept loop
// ---------------------------------------------------------------------------

void AIRemoteServer::_accept_thread_func(void *p_userdata) {
	static_cast<AIRemoteServer *>(p_userdata)->_accept_loop();
}

void AIRemoteServer::_accept_loop() {
	while (!accept_exit.is_set()) {
		if (server.is_null() || !server->is_listening()) {
			break;
		}
		if (!server->is_connection_available()) {
			OS::get_singleton()->delay_usec(20000); // 20 ms
			continue;
		}

		Ref<StreamPeerTCP> peer = server->take_connection();
		if (peer.is_null()) {
			continue;
		}

		const IPAddress host = peer->get_connected_host();
		const String host_str = String(host);

		if (!_peer_allowed(host)) {
			// Never respond to a non-private peer — no banner, no error page.
			WARN_PRINT(vformat("AI Remote: rejected connection from non-private address %s.", host_str));
			Dictionary det;
			det["peer"] = host_str;
			AIRemoteDeviceStore::audit("rejected_peer", det);
			peer->disconnect_from_host();
			continue;
		}
		if (_is_throttled(host_str)) {
			peer->disconnect_from_host();
			continue;
		}

		int slot = -1;
		for (int i = 0; i < MAX_CONNECTIONS; i++) {
			if (!conns[i].in_use.is_set()) {
				slot = i;
				break;
			}
		}
		if (slot < 0) {
			peer->disconnect_from_host();
			continue;
		}

		Conn *c = &conns[slot];
		if (c->thread.is_started()) {
			c->thread.wait_to_finish();
		}
		c->tcp = peer;
		c->client_role = false;
		c->tls.unref();
		c->stream.unref();
		c->stop.clear();
		c->phase = CONN_HTTP;
		c->rx = PackedByteArray();
		c->assembler.reset();
		c->channel.reset();
		c->authenticated = false;
		c->can_control = false;
		c->device_id = String();
		c->device_name = String();
		c->peer_desc = host_str;
		c->connected_at_ms = _now_ms();
		c->last_activity_ms = c->connected_at_ms;
		{
			MutexLock lock(c->outbox_mutex);
			c->outbox.clear();
		}
		c->in_use.set();
		c->thread.start(_conn_thread_func, c);
	}
}

// ---------------------------------------------------------------------------
// Connection thread
// ---------------------------------------------------------------------------

void AIRemoteServer::_conn_thread_func(void *p_userdata) {
	Conn *c = static_cast<Conn *>(p_userdata);
	AIRemoteServer *self = AIRemoteServer::get_singleton();
	if (self) {
		self->_conn_loop(c);
	}
	c->in_use.clear();
}

void AIRemoteServer::_conn_loop(Conn *p_conn) {
	// --- TLS (LAN mode) ---
	if (use_tls) {
		p_conn->tls = Ref<StreamPeerTLS>(StreamPeerTLS::create());
		if (p_conn->tls.is_null() || p_conn->tls->accept_stream(p_conn->tcp, tls_options) != OK) {
			p_conn->tcp->disconnect_from_host();
			return;
		}
		const uint64_t deadline = _now_ms() + HANDSHAKE_TIMEOUT_MS;
		while (!p_conn->stop.is_set()) {
			p_conn->tls->poll();
			const StreamPeerTLS::Status st = p_conn->tls->get_status();
			if (st == StreamPeerTLS::STATUS_CONNECTED) {
				break;
			}
			if (st != StreamPeerTLS::STATUS_HANDSHAKING || _now_ms() > deadline) {
				p_conn->tcp->disconnect_from_host();
				return;
			}
			OS::get_singleton()->delay_usec(5000);
		}
		p_conn->stream = p_conn->tls;
	} else {
		p_conn->stream = p_conn->tcp;
	}

	// --- HTTP request ---
	String method, path;
	HashMap<String, String> headers;
	if (!_read_http_request(p_conn, method, path, headers)) {
		_close_conn(p_conn, 1002, "bad request");
		return;
	}

	const bool wants_upgrade = headers.has("upgrade") &&
			headers["upgrade"].to_lower().contains("websocket");

	if (!wants_upgrade) {
		if (method != "GET") {
			_send_http(p_conn, 405, "Method Not Allowed", "text/plain", (const uint8_t *)"", 0);
		} else {
			_serve_file(p_conn, path);
		}
		p_conn->tcp->disconnect_from_host();
		return;
	}

	if (!_do_ws_handshake(p_conn, headers)) {
		p_conn->tcp->disconnect_from_host();
		return;
	}
	p_conn->phase = CONN_WS_OPEN;
	p_conn->last_activity_ms = _now_ms();

	_ws_pump(p_conn);

	p_conn->tcp->disconnect_from_host();
	p_conn->stream.unref();
	p_conn->tls.unref();
	p_conn->channel.reset();
	p_conn->phase = CONN_FREE;
}

void AIRemoteServer::_send_ws(Conn *p_conn, int p_opcode, const PackedByteArray &p_payload) {
	if (p_conn->stream.is_null()) {
		return;
	}
	PackedByteArray frame;
	if (p_conn->client_role) {
		// A WebSocket client must mask every frame with fresh random bytes.
		PackedByteArray mask = AIRemoteCrypto::random_bytes(4);
		if (mask.size() != 4) {
			return;
		}
		frame = AIRemoteWS::build_frame(p_opcode, p_payload, true, mask.ptr());
	} else {
		frame = AIRemoteWS::build_frame(p_opcode, p_payload, true, nullptr);
	}
	if (p_conn->stream->put_data(frame.ptr(), frame.size()) != OK) {
		p_conn->stop.set();
	}
}

void AIRemoteServer::_ws_pump(Conn *p_conn) {
	while (!p_conn->stop.is_set() && running.is_set()) {
		if (p_conn->tcp->get_status() != StreamPeerTCP::STATUS_CONNECTED) {
			break;
		}
		p_conn->tcp->poll();
		if (p_conn->tls.is_valid()) {
			p_conn->tls->poll();
		}

		// Read whatever is available.
		int available = p_conn->stream->get_available_bytes();
		if (available > 0) {
			PackedByteArray chunk;
			chunk.resize(available);
			int received = 0;
			if (p_conn->stream->get_partial_data(chunk.ptrw(), available, received) != OK) {
				break;
			}
			if (received > 0) {
				chunk.resize(received);
				p_conn->rx.append_array(chunk);
			}
		}

		// Drain complete frames.
		bool fatal = false;
		while (p_conn->rx.size() > 0) {
			AIRemoteWS::Frame frame;
			AIRemoteWS::ParseResult res = AIRemoteWS::parse_frame(p_conn->rx, 0, frame, !p_conn->client_role);
			if (res == AIRemoteWS::PARSE_INCOMPLETE) {
				break;
			}
			if (res == AIRemoteWS::PARSE_ERROR) {
				fatal = true;
				break;
			}
			p_conn->rx = p_conn->rx.slice(frame.consumed);

			PackedByteArray message;
			int opcode = 0;
			bool assembly_error = false;
			if (p_conn->assembler.feed(frame, message, opcode, assembly_error)) {
				_handle_ws_message(p_conn, message, opcode);
			}
			if (assembly_error) {
				fatal = true;
				break;
			}
		}
		if (fatal) {
			_close_conn(p_conn, 1002, "protocol error");
			break;
		}

		_flush_outbox(p_conn);

		// Session lifetime limits.
		const uint64_t now = _now_ms();
		if (p_conn->authenticated) {
			if (now - p_conn->last_activity_ms > (uint64_t)SESSION_IDLE_MS ||
					now - p_conn->authed_at_ms > (uint64_t)SESSION_ABSOLUTE_MS) {
				_close_conn(p_conn, 1000, "session expired");
				break;
			}
			// A revoked device loses its live session immediately.
			MutexLock lock(devices_mutex);
			if (!devices.find_by_id(p_conn->device_id)) {
				_close_conn(p_conn, 1008, "device revoked");
				break;
			}
		} else if (!p_conn->client_role && now - p_conn->connected_at_ms > (uint64_t)HANDSHAKE_TIMEOUT_MS) {
			// Not applied in relay mode: the socket legitimately sits idle
			// until a guest joins the room.
			_close_conn(p_conn, 1008, "handshake timeout");
			break;
		}

		OS::get_singleton()->delay_usec(10000); // 10 ms
	}
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

bool AIRemoteServer::_read_http_request(Conn *p_conn, String &r_method, String &r_path,
		HashMap<String, String> &r_headers) {
	PackedByteArray buf;
	const uint64_t deadline = _now_ms() + HANDSHAKE_TIMEOUT_MS;
	const int MAX_REQUEST = 16 * 1024;

	while (!p_conn->stop.is_set()) {
		if (_now_ms() > deadline || buf.size() > MAX_REQUEST) {
			return false;
		}
		p_conn->tcp->poll();
		if (p_conn->tls.is_valid()) {
			p_conn->tls->poll();
		}
		if (p_conn->tcp->get_status() != StreamPeerTCP::STATUS_CONNECTED) {
			return false;
		}

		const int available = p_conn->stream->get_available_bytes();
		if (available <= 0) {
			OS::get_singleton()->delay_usec(5000);
			continue;
		}
		PackedByteArray chunk;
		chunk.resize(available);
		int received = 0;
		if (p_conn->stream->get_partial_data(chunk.ptrw(), available, received) != OK) {
			return false;
		}
		chunk.resize(received);
		buf.append_array(chunk);

		// Find the header terminator.
		for (int i = 3; i < buf.size(); i++) {
			if (buf[i - 3] == '\r' && buf[i - 2] == '\n' && buf[i - 1] == '\r' && buf[i] == '\n') {
				String text = String::utf8((const char *)buf.ptr(), i - 3);
				Vector<String> lines = text.split("\r\n", false);
				if (lines.is_empty()) {
					return false;
				}
				Vector<String> req = lines[0].split(" ", false);
				if (req.size() < 2) {
					return false;
				}
				r_method = req[0].to_upper();
				r_path = req[1];
				for (int l = 1; l < lines.size(); l++) {
					const int colon = lines[l].find_char(':');
					if (colon <= 0) {
						continue;
					}
					const String name = lines[l].substr(0, colon).strip_edges().to_lower();
					const String value = lines[l].substr(colon + 1).strip_edges();
					r_headers[name] = value;
				}
				// Anything after the headers belongs to the WebSocket stream.
				if (i + 1 < buf.size()) {
					p_conn->rx = buf.slice(i + 1);
				}
				return true;
			}
		}
	}
	return false;
}

void AIRemoteServer::_send_http(Conn *p_conn, int p_code, const String &p_status, const String &p_mime,
		const uint8_t *p_body, int p_len) {
	String head = vformat("HTTP/1.1 %d %s\r\n", p_code, p_status);
	head += vformat("Content-Type: %s\r\n", p_mime);
	head += vformat("Content-Length: %d\r\n", p_len);
	head += "Connection: close\r\n";
	// The client is a self-contained page talking only to its own origin.
	head += "Cache-Control: no-store\r\n";
	head += "X-Content-Type-Options: nosniff\r\n";
	head += "Referrer-Policy: no-referrer\r\n";
	head += "Content-Security-Policy: default-src 'none'; script-src 'self'; style-src 'self'; "
			"img-src 'self' data:; connect-src 'self' ws: wss:; base-uri 'none'; form-action 'none'\r\n";
	head += "\r\n";

	CharString cs = head.utf8();
	p_conn->stream->put_data((const uint8_t *)cs.get_data(), cs.length());
	if (p_len > 0) {
		p_conn->stream->put_data(p_body, p_len);
	}
}

void AIRemoteServer::_serve_file(Conn *p_conn, const String &p_path) {
	String name = p_path;
	const int query = name.find_char('?');
	if (query >= 0) {
		name = name.substr(0, query);
	}
	if (name == "/" || name.is_empty()) {
		name = "index.html";
	} else {
		name = name.trim_prefix("/");
	}
	// No path traversal: only exact matches against the embedded table.
	for (int i = 0; i < AI_WEBCLIENT_FILE_COUNT; i++) {
		if (name == AI_WEBCLIENT_FILES[i].name) {
			_send_http(p_conn, 200, "OK", AI_WEBCLIENT_FILES[i].mime,
					AI_WEBCLIENT_FILES[i].data, AI_WEBCLIENT_FILES[i].size);
			return;
		}
	}
	const char *body = "Not found";
	_send_http(p_conn, 404, "Not Found", "text/plain", (const uint8_t *)body, (int)strlen(body));
}

bool AIRemoteServer::_do_ws_handshake(Conn *p_conn, const HashMap<String, String> &p_headers) {
	if (!p_headers.has("sec-websocket-key")) {
		return false;
	}
	const String version = p_headers.has("sec-websocket-version") ? p_headers["sec-websocket-version"] : String();
	if (version != "13") {
		return false;
	}
	const String accept = AIRemoteWS::compute_accept_key(p_headers["sec-websocket-key"]);
	if (accept.is_empty()) {
		return false;
	}

	String resp = "HTTP/1.1 101 Switching Protocols\r\n";
	resp += "Upgrade: websocket\r\n";
	resp += "Connection: Upgrade\r\n";
	resp += vformat("Sec-WebSocket-Accept: %s\r\n\r\n", accept);
	CharString cs = resp.utf8();
	return p_conn->stream->put_data((const uint8_t *)cs.get_data(), cs.length()) == OK;
}

// ---------------------------------------------------------------------------
// WebSocket messages
// ---------------------------------------------------------------------------

void AIRemoteServer::_handle_ws_message(Conn *p_conn, const PackedByteArray &p_payload, int p_opcode) {
	switch (p_opcode) {
		case AIRemoteWS::OP_CLOSE: {
			p_conn->stop.set();
			return;
		}
		case AIRemoteWS::OP_PING: {
			_send_ws(p_conn, AIRemoteWS::OP_PONG, p_payload);
			return;
		}
		case AIRemoteWS::OP_PONG: {
			return;
		}
		case AIRemoteWS::OP_TEXT: {
			// Only the handshake travels as plain text. Once the channel is up,
			// text frames are not part of the protocol.
			if (p_conn->authenticated) {
				_close_conn(p_conn, 1002, "unexpected text frame");
				return;
			}
			JSON json;
			if (json.parse(String::utf8((const char *)p_payload.ptr(), p_payload.size())) != OK ||
					json.get_data().get_type() != Variant::DICTIONARY) {
				_close_conn(p_conn, 1002, "bad json");
				return;
			}
			Dictionary msg = json.get_data();
			if (p_conn->client_role && _handle_relay_control(p_conn, msg)) {
				return;
			}
			_handle_plain_json(p_conn, msg);
			return;
		}
		case AIRemoteWS::OP_BINARY: {
			PackedByteArray plain;
			if (!p_conn->channel.is_established() || !p_conn->channel.decrypt(p_payload, plain)) {
				// Authentication failure, replay, or a stale counter.
				_note_failure(p_conn->peer_desc);
				_close_conn(p_conn, 1008, "decrypt failed");
				return;
			}
			JSON json;
			if (json.parse(String::utf8((const char *)plain.ptr(), plain.size())) != OK ||
					json.get_data().get_type() != Variant::DICTIONARY) {
				_close_conn(p_conn, 1002, "bad json");
				return;
			}
			_handle_encrypted_json(p_conn, json.get_data());
			return;
		}
		default:
			_close_conn(p_conn, 1002, "bad opcode");
			return;
	}
}

void AIRemoteServer::_handle_plain_json(Conn *p_conn, const Dictionary &p_msg) {
	const String t = p_msg.get("t", "");

	// ---- Pairing ----
	if (t == "pair_hello") {
		PackedByteArray secret;
		{
			MutexLock lock(pairing_mutex);
			if (pairing_secret.is_empty() || _now_ms() > pairing_expires_ms) {
				secret = PackedByteArray();
			} else {
				secret = pairing_secret;
			}
		}
		if (secret.is_empty()) {
			Dictionary err;
			err["t"] = "error";
			err["message"] = "No pairing window is open. Start pairing in the editor.";
			_send_plain_json(p_conn, err);
			return;
		}

		PackedByteArray dev_pk = AIRemoteCrypto::b64_decode(p_msg.get("dev_pk", ""));
		if (dev_pk.size() != AIRemoteCrypto::X25519_KEY_SIZE) {
			_close_conn(p_conn, 1002, "bad key");
			return;
		}
		p_conn->device_public = dev_pk;
		p_conn->device_name = String(p_msg.get("name", "device")).substr(0, 64);

		p_conn->nonce_server = AIRemoteCrypto::random_bytes(32); // reused as the pair salt
		Dictionary ack;
		ack["t"] = "pair_ack";
		{
			MutexLock lock(devices_mutex);
			ack["srv_pk"] = AIRemoteCrypto::b64_encode(devices.get_server_public());
		}
		ack["salt"] = AIRemoteCrypto::b64_encode(p_conn->nonce_server);
		_send_plain_json(p_conn, ack);
		return;
	}

	if (t == "pair_confirm") {
		PackedByteArray secret;
		bool window_open = false;
		{
			MutexLock lock(pairing_mutex);
			window_open = !pairing_secret.is_empty() && _now_ms() <= pairing_expires_ms;
			secret = pairing_secret;
			if (window_open) {
				pairing_attempts++;
				if (pairing_attempts > PAIRING_MAX_ATTEMPTS) {
					pairing_secret = PackedByteArray();
					window_open = false;
				}
			}
		}
		if (!window_open || p_conn->device_public.is_empty()) {
			_close_conn(p_conn, 1008, "pairing unavailable");
			return;
		}

		PackedByteArray key;
		{
			MutexLock lock(devices_mutex);
			key = AIRemoteProtocol::derive_pair_key(devices.get_server_private(),
					p_conn->device_public, secret, p_conn->nonce_server);
		}
		PackedByteArray proof = AIRemoteCrypto::b64_decode(p_msg.get("proof", ""));
		PackedByteArray expected;
		{
			MutexLock lock(devices_mutex);
			expected = AIRemoteProtocol::pair_proof_plaintext(p_conn->device_public, devices.get_server_public());
		}

		PackedByteArray got;
		const PackedByteArray nonce = AIRemoteProtocol::make_nonce(AIRemoteProtocol::DIR_PAIR, 0);
		if (key.is_empty() ||
				!AIRemoteCrypto::aes_gcm_decrypt(key, nonce, proof, PackedByteArray(), got) ||
				!AIRemoteCrypto::const_time_equals(got, expected)) {
			_note_failure(p_conn->peer_desc);
			Dictionary err;
			err["t"] = "error";
			err["code"] = "bad_pairing_code";
			err["message"] = "Pairing code did not match.";
			_send_plain_json(p_conn, err);
			Dictionary det;
			det["peer"] = p_conn->peer_desc;
			AIRemoteDeviceStore::audit("pairing_failed", det);
			return;
		}

		String id;
		{
			MutexLock lock(devices_mutex);
			id = devices.add_device(p_conn->device_public, p_conn->device_name);
		}
		if (id.is_empty()) {
			_close_conn(p_conn, 1011, "pairing failed");
			return;
		}
		// One pairing per window: burn the secret immediately.
		{
			MutexLock lock(pairing_mutex);
			pairing_secret = PackedByteArray();
			pairing_expires_ms = 0;
		}

		Dictionary ok;
		ok["t"] = "pair_ok";
		ok["device_id"] = id;
		_send_plain_json(p_conn, ok);

		call_deferred(SNAME("emit_signal"), SNAME("devices_changed"));
		return;
	}

	// ---- Session handshake ----
	if (t == "hello") {
		PackedByteArray dev_pk = AIRemoteCrypto::b64_decode(p_msg.get("dev_pk", ""));
		PackedByteArray eph_pk = AIRemoteCrypto::b64_decode(p_msg.get("eph_pk", ""));
		PackedByteArray nonce_c = AIRemoteCrypto::b64_decode(p_msg.get("nonce_c", ""));
		if (dev_pk.size() != AIRemoteCrypto::X25519_KEY_SIZE ||
				eph_pk.size() != AIRemoteCrypto::X25519_KEY_SIZE || nonce_c.size() != 32) {
			_close_conn(p_conn, 1002, "bad handshake");
			return;
		}

		String device_id;
		String device_name;
		bool allow_control = false;
		PackedByteArray server_private;
		{
			MutexLock lock(devices_mutex);
			const AIRemoteDevice *dev = devices.find_by_public_key(dev_pk);
			const int64_t now_wall = (int64_t)(Time::get_singleton()->get_unix_time_from_system() * 1000.0);
			if (!dev || dev->is_expired(now_wall)) {
				device_id = String();
			} else {
				device_id = dev->id;
				device_name = dev->name;
				allow_control = dev->allow_control;
			}
			server_private = devices.get_server_private();
		}
		if (device_id.is_empty()) {
			_note_failure(p_conn->peer_desc);
			Dictionary err;
			err["t"] = "error";
			// Machine-readable so the client can drop its stored keys without
			// pattern-matching on human-facing copy.
			err["code"] = "unknown_device";
			err["message"] = "This device is not paired. Pair it again from the editor.";
			_send_plain_json(p_conn, err);
			Dictionary det;
			det["peer"] = p_conn->peer_desc;
			AIRemoteDeviceStore::audit("unknown_device_rejected", det);
			return;
		}

		if (!AIRemoteCrypto::generate_keypair(p_conn->ephemeral_private, p_conn->ephemeral_public)) {
			_close_conn(p_conn, 1011, "key generation failed");
			return;
		}
		p_conn->device_public = dev_pk;
		p_conn->nonce_client = nonce_c;
		p_conn->nonce_server = AIRemoteCrypto::random_bytes(32);

		PackedByteArray k_c2s, k_s2c;
		if (!AIRemoteProtocol::derive_session_keys(true, server_private, p_conn->ephemeral_private,
					dev_pk, eph_pk, p_conn->nonce_client, p_conn->nonce_server, k_c2s, k_s2c)) {
			_close_conn(p_conn, 1011, "key agreement failed");
			return;
		}

		Dictionary ack;
		ack["t"] = "hello_ack";
		ack["eph_pk"] = AIRemoteCrypto::b64_encode(p_conn->ephemeral_public);
		ack["nonce_s"] = AIRemoteCrypto::b64_encode(p_conn->nonce_server);
		_send_plain_json(p_conn, ack);

		// The channel is armed, but the connection is not authenticated until a
		// frame actually decrypts — that is the proof the peer holds the key.
		p_conn->channel.establish(k_s2c, k_c2s, AIRemoteProtocol::DIR_S2C, AIRemoteProtocol::DIR_C2S);
		p_conn->device_id = device_id;
		p_conn->device_name = device_name;
		p_conn->can_control = allow_control;
		return;
	}

	_close_conn(p_conn, 1002, "unexpected message");
}

void AIRemoteServer::_handle_encrypted_json(Conn *p_conn, const Dictionary &p_msg) {
	const String t = p_msg.get("t", "");

	if (!p_conn->authenticated) {
		// First frame that decrypts completes authentication.
		if (t != "hello_verify") {
			_close_conn(p_conn, 1008, "expected hello_verify");
			return;
		}
		p_conn->authenticated = true;
		p_conn->authed_at_ms = _now_ms();

		{
			MutexLock lock(devices_mutex);
			devices.touch(p_conn->device_id, (int64_t)(Time::get_singleton()->get_unix_time_from_system() * 1000.0));
			devices.save();
		}
		Dictionary det;
		det["device_id"] = p_conn->device_id;
		det["peer"] = p_conn->peer_desc;
		AIRemoteDeviceStore::audit("session_established", det);

		Dictionary ready;
		ready["t"] = "ready";
		_queue_json(p_conn, ready);
		// The client asks for state and history itself once it sees "ready";
		// sending them unprompted here would ship the transcript twice.
		return;
	}

	if (t == "ping") {
		// Deliberately does NOT count as activity: a phone left open should
		// still hit the idle limit and re-handshake. Reconnection is automatic
		// and uses the stored device key, so this is invisible to the user.
		Dictionary pong;
		pong["t"] = "pong";
		_queue_json(p_conn, pong);
		return;
	}

	p_conn->last_activity_ms = _now_ms();

	// Commands that change anything require an explicit control grant made on
	// the desktop. Pairing alone is view-only.
	const bool mutating = (t == "send" || t == "cancel" || t == "approve" ||
			t == "switch_chat" || t == "new_chat");
	if (mutating) {
		bool allowed = false;
		{
			MutexLock lock(devices_mutex);
			const AIRemoteDevice *dev = devices.find_by_id(p_conn->device_id);
			allowed = dev && dev->allow_control;
		}
		p_conn->can_control = allowed;
		if (!allowed) {
			Dictionary err;
			err["t"] = "error";
			err["code"] = "view_only";
			err["message"] = "This device is view-only. Grant control in the editor's Remote Access panel.";
			_queue_json(p_conn, err);
			return;
		}
		Dictionary det;
		det["device_id"] = p_conn->device_id;
		det["command"] = t;
		if (t == "send") {
			det["chars"] = String(p_msg.get("text", "")).length();
		}
		AIRemoteDeviceStore::audit("remote_command", det);
	}

	callable_mp(this, &AIRemoteServer::_dispatch_command).call_deferred(p_conn->index, p_msg);
}

void AIRemoteServer::_send_plain_json(Conn *p_conn, const Dictionary &p_msg) {
	CharString cs = JSON::stringify(p_msg).utf8();
	PackedByteArray payload;
	payload.resize(cs.length());
	if (cs.length() > 0) {
		memcpy(payload.ptrw(), cs.get_data(), cs.length());
	}
	_send_ws(p_conn, AIRemoteWS::OP_TEXT, payload);
}

void AIRemoteServer::_queue_json(Conn *p_conn, const Dictionary &p_msg) {
	CharString cs = JSON::stringify(p_msg).utf8();
	PackedByteArray payload;
	payload.resize(cs.length());
	if (cs.length() > 0) {
		memcpy(payload.ptrw(), cs.get_data(), cs.length());
	}
	MutexLock lock(p_conn->outbox_mutex);
	p_conn->outbox.push_back(payload);
}

void AIRemoteServer::_flush_outbox(Conn *p_conn) {
	List<PackedByteArray> pending;
	{
		MutexLock lock(p_conn->outbox_mutex);
		if (p_conn->outbox.is_empty()) {
			return;
		}
		pending = p_conn->outbox;
		p_conn->outbox.clear();
	}
	for (const PackedByteArray &payload : pending) {
		if (!p_conn->channel.is_established()) {
			return;
		}
		PackedByteArray sealed = p_conn->channel.encrypt(payload);
		if (sealed.is_empty()) {
			continue;
		}
		_send_ws(p_conn, AIRemoteWS::OP_BINARY, sealed);
		if (p_conn->stop.is_set()) {
			return;
		}
	}
}

void AIRemoteServer::_close_conn(Conn *p_conn, int p_code, const String &p_reason) {
	if (p_conn->stream.is_valid()) {
		PackedByteArray payload;
		payload.resize(2);
		payload.write[0] = (uint8_t)((p_code >> 8) & 0xFF);
		payload.write[1] = (uint8_t)(p_code & 0xFF);
		payload.append_array(p_reason.to_utf8_buffer());
		_send_ws(p_conn, AIRemoteWS::OP_CLOSE, payload);
	}
	p_conn->stop.set();
}

// ---------------------------------------------------------------------------
// Relay transport (outbound; no local port is opened)
// ---------------------------------------------------------------------------

Error AIRemoteServer::start_relay(const String &p_url) {
	if (running.is_set()) {
		return ERR_ALREADY_IN_USE;
	}
	if (!p_url.begins_with("ws://") && !p_url.begins_with("wss://")) {
		ERR_PRINT("AIRemoteServer: relay URL must start with ws:// or wss://");
		return ERR_INVALID_PARAMETER;
	}

	{
		MutexLock lock(devices_mutex);
		devices.load();
		if (!devices.ensure_identity()) {
			return FAILED;
		}
	}

	relay_mode = true;
	relay_url = p_url;
	use_tls = false; // the app-layer channel is the security boundary here
	running.set();
	relay_exit.clear();
	relay_thread.start(_relay_thread_func, this);

	_connect_session();

	Dictionary det;
	det["url"] = p_url;
	AIRemoteDeviceStore::audit("relay_started", det);
	print_line(vformat("AI Remote: relay transport dialling %s", p_url));
	emit_signal(SNAME("state_changed"));
	return OK;
}

void AIRemoteServer::_relay_thread_func(void *p_userdata) {
	static_cast<AIRemoteServer *>(p_userdata)->_relay_loop();
}

void AIRemoteServer::_relay_loop() {
	// Slot 0 carries the relay connection; the other slots stay unused because
	// a relay room holds exactly one guest at a time.
	Conn *c = &conns[0];
	int backoff_ms = 1000;

	while (!relay_exit.is_set() && running.is_set()) {
		c->stop.clear();
		c->client_role = true;
		c->rx = PackedByteArray();
		c->assembler.reset();
		c->channel.reset();
		c->authenticated = false;
		c->can_control = false;
		c->device_id = String();
		c->peer_desc = relay_url;
		c->connected_at_ms = _now_ms();
		c->last_activity_ms = c->connected_at_ms;
		{
			MutexLock lock(c->outbox_mutex);
			c->outbox.clear();
		}

		if (_relay_connect(c, relay_url)) {
			backoff_ms = 1000; // a successful dial resets the backoff
			c->in_use.set();
			_ws_pump(c);
			c->in_use.clear();
		}

		if (c->tcp.is_valid()) {
			c->tcp->disconnect_from_host();
		}
		c->stream.unref();
		c->tls.unref();
		c->tcp.unref();
		c->channel.reset();

		if (relay_exit.is_set() || !running.is_set()) {
			break;
		}
		// A relay link is expected to drop; back off rather than hammer it.
		for (int waited = 0; waited < backoff_ms && !relay_exit.is_set(); waited += 100) {
			OS::get_singleton()->delay_usec(100000);
		}
		backoff_ms = MIN(backoff_ms * 2, 30000);
	}
}

bool AIRemoteServer::_relay_connect(Conn *p_conn, const String &p_url) {
	const bool secure = p_url.begins_with("wss://");
	String rest = p_url.substr(secure ? 6 : 5);

	String hostport = rest;
	String path = "/";
	const int slash = rest.find_char('/');
	if (slash >= 0) {
		hostport = rest.substr(0, slash);
		path = rest.substr(slash);
	}
	String host = hostport;
	int rport = secure ? 443 : 80;
	const int colon = hostport.rfind_char(':');
	if (colon >= 0) {
		host = hostport.substr(0, colon);
		rport = hostport.substr(colon + 1).to_int();
	}
	if (host.is_empty() || rport <= 0) {
		return false;
	}

	IPAddress resolved;
	if (host.is_valid_ip_address()) {
		resolved = IPAddress(host);
	} else {
		resolved = IP::get_singleton()->resolve_hostname(host);
	}
	if (!resolved.is_valid()) {
		return false;
	}

	p_conn->tcp = Ref<StreamPeerTCP>(memnew(StreamPeerTCP));
	if (p_conn->tcp->connect_to_host(resolved, rport) != OK) {
		return false;
	}
	const uint64_t deadline = _now_ms() + HANDSHAKE_TIMEOUT_MS;
	while (p_conn->tcp->get_status() == StreamPeerTCP::STATUS_CONNECTING) {
		if (_now_ms() > deadline || p_conn->stop.is_set()) {
			return false;
		}
		p_conn->tcp->poll();
		OS::get_singleton()->delay_usec(10000);
	}
	if (p_conn->tcp->get_status() != StreamPeerTCP::STATUS_CONNECTED) {
		return false;
	}

	if (secure) {
		p_conn->tls = Ref<StreamPeerTLS>(StreamPeerTLS::create());
		if (p_conn->tls.is_null() ||
				p_conn->tls->connect_to_stream(p_conn->tcp, host, TLSOptions::client()) != OK) {
			return false;
		}
		while (true) {
			p_conn->tls->poll();
			const StreamPeerTLS::Status st = p_conn->tls->get_status();
			if (st == StreamPeerTLS::STATUS_CONNECTED) {
				break;
			}
			if (st != StreamPeerTLS::STATUS_HANDSHAKING || _now_ms() > deadline) {
				return false;
			}
			OS::get_singleton()->delay_usec(5000);
		}
		p_conn->stream = p_conn->tls;
	} else {
		p_conn->stream = p_conn->tcp;
	}

	// Client-role WebSocket handshake.
	PackedByteArray nonce = AIRemoteCrypto::random_bytes(16);
	const String key = AIRemoteCrypto::b64_encode(nonce);
	String req = vformat("GET %s HTTP/1.1\r\n", path);
	req += vformat("Host: %s\r\n", hostport);
	req += "Upgrade: websocket\r\nConnection: Upgrade\r\n";
	req += vformat("Sec-WebSocket-Key: %s\r\n", key);
	req += "Sec-WebSocket-Version: 13\r\n\r\n";
	CharString cs = req.utf8();
	if (p_conn->stream->put_data((const uint8_t *)cs.get_data(), cs.length()) != OK) {
		return false;
	}

	// Read the 101 response.
	PackedByteArray buf;
	while (true) {
		if (_now_ms() > deadline || p_conn->stop.is_set() || buf.size() > 8192) {
			return false;
		}
		p_conn->tcp->poll();
		if (p_conn->tls.is_valid()) {
			p_conn->tls->poll();
		}
		const int available = p_conn->stream->get_available_bytes();
		if (available <= 0) {
			OS::get_singleton()->delay_usec(5000);
			continue;
		}
		PackedByteArray chunk;
		chunk.resize(available);
		int received = 0;
		if (p_conn->stream->get_partial_data(chunk.ptrw(), available, received) != OK) {
			return false;
		}
		chunk.resize(received);
		buf.append_array(chunk);

		for (int i = 3; i < buf.size(); i++) {
			if (buf[i - 3] == '\r' && buf[i - 2] == '\n' && buf[i - 1] == '\r' && buf[i] == '\n') {
				const String text = String::utf8((const char *)buf.ptr(), i - 3);
				if (!text.begins_with("HTTP/1.1 101")) {
					return false;
				}
				// Verifying the accept key proves we reached a real WebSocket
				// endpoint rather than a proxy that echoed a 101.
				const String expected = AIRemoteWS::compute_accept_key(key);
				if (!text.to_lower().contains(expected.to_lower())) {
					return false;
				}
				if (i + 1 < buf.size()) {
					p_conn->rx = buf.slice(i + 1);
				}
				p_conn->phase = CONN_WS_OPEN;
				return true;
			}
		}
	}
}

bool AIRemoteServer::_handle_relay_control(Conn *p_conn, const Dictionary &p_msg) {
	const String t = p_msg.get("t", "");
	if (t == "relay_hello" || t == "peer_here") {
		return true; // nothing to do; the guest drives the handshake
	}
	if (t == "peer_gone") {
		// The guest left. Tear down the crypto session so the next guest starts
		// a fresh handshake, but keep the relay socket connected.
		p_conn->channel.reset();
		p_conn->assembler.reset();
		p_conn->authenticated = false;
		p_conn->can_control = false;
		p_conn->device_id = String();
		p_conn->connected_at_ms = _now_ms();
		{
			MutexLock lock(p_conn->outbox_mutex);
			p_conn->outbox.clear();
		}
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Main-thread bridge
// ---------------------------------------------------------------------------

Dictionary AIRemoteServer::_state_dict(bool p_can_control) const {
	Dictionary d;
	d["t"] = "state";
	AIChatSession *session = AIChatSession::get_singleton();
	if (session) {
		d["chat_id"] = session->get_chat_id();
		d["running"] = session->is_running();
		d["policy_mode"] = session->get_policy_mode();
		d["status"] = session->get_status_text();
		Dictionary approval = session->get_pending_approval();
		d["approval"] = approval.is_empty() ? Variant() : Variant(approval);
	} else {
		d["chat_id"] = "";
		d["running"] = false;
		d["policy_mode"] = 0;
		d["status"] = "";
		d["approval"] = Variant();
	}
	d["can_control"] = p_can_control;
	return d;
}

// Strips inline base64 images: a transcript with screenshots is many megabytes
// and would stall a phone. The tool card still shows that an image exists.
Dictionary AIRemoteServer::_sanitize_item(const Dictionary &p_item) {
	Dictionary item = p_item.duplicate(true);
	if (item.has("images")) {
		Array imgs = item["images"];
		item.erase("images");
		item["images_omitted"] = imgs.size();
	}
	if (item.has("content") && item["content"].get_type() == Variant::DICTIONARY) {
		Dictionary content = item["content"];
		if (content.has("result") && content["result"].get_type() == Variant::DICTIONARY) {
			Dictionary result = content["result"];
			if (result.has("screenshot_b64")) {
				result.erase("screenshot_b64");
				result["screenshot_omitted"] = true;
			}
			if (result.has("screenshots_b64")) {
				result.erase("screenshots_b64");
				result["screenshot_omitted"] = true;
			}
			content["result"] = result;
			item["content"] = content;
		}
	}
	return item;
}

void AIRemoteServer::_dispatch_command(int p_conn_index, const Dictionary &p_msg) {
	ERR_FAIL_INDEX(p_conn_index, MAX_CONNECTIONS);
	Conn *c = &conns[p_conn_index];
	if (!c->in_use.is_set()) {
		return;
	}

	AIChatSession *session = AIChatSession::get_singleton();
	const String t = p_msg.get("t", "");

	if (t == "get_state") {
		bool can_control = false;
		{
			MutexLock lock(devices_mutex);
			const AIRemoteDevice *dev = devices.find_by_id(c->device_id);
			can_control = dev && dev->allow_control;
		}
		_queue_json(c, _state_dict(can_control));
		return;
	}

	if (t == "get_history") {
		Dictionary msg;
		msg["t"] = "history";
		msg["chat_id"] = session ? session->get_chat_id() : String();
		Array items;
		if (session) {
			Array raw = session->get_history();
			for (int i = 0; i < raw.size(); i++) {
				Dictionary row = raw[i];
				Dictionary out;
				out["ts"] = row.get("ts", 0);
				out["item"] = _sanitize_item(row.get("item", Dictionary()));
				items.push_back(out);
			}
		}
		msg["items"] = items;
		_queue_json(c, msg);
		return;
	}

	if (t == "list_chats") {
		Dictionary msg;
		msg["t"] = "chats";
		msg["chats"] = session ? session->list_chats() : Array();
		_queue_json(c, msg);
		return;
	}

	if (!session) {
		Dictionary err;
		err["t"] = "error";
		err["message"] = "The editor session is unavailable.";
		_queue_json(c, err);
		return;
	}

	Error result = ERR_UNAVAILABLE;
	if (t == "send") {
		result = session->submit_user_message(p_msg.get("text", ""));
	} else if (t == "cancel") {
		result = session->cancel_run();
	} else if (t == "approve") {
		result = session->respond_approval(p_msg.get("decision", ""));
	} else if (t == "switch_chat") {
		result = session->switch_chat(p_msg.get("id", ""));
	} else if (t == "new_chat") {
		result = session->new_chat();
	} else {
		Dictionary err;
		err["t"] = "error";
		err["message"] = vformat("Unknown command '%s'.", t);
		_queue_json(c, err);
		return;
	}

	if (result != OK) {
		Dictionary err;
		err["t"] = "error";
		err["message"] = result == ERR_UNAVAILABLE
				? "The editor cannot accept that right now."
				: "That command was rejected.";
		_queue_json(c, err);
	}
}

void AIRemoteServer::_connect_session() {
	AIChatSession *session = AIChatSession::get_singleton();
	if (!session || session_connected) {
		return;
	}
	session->connect("item_appended", callable_mp(this, &AIRemoteServer::_on_session_item_appended));
	session->connect("history_changed", callable_mp(this, &AIRemoteServer::_on_session_history_changed));
	session->connect("chat_switched", callable_mp(this, &AIRemoteServer::_on_session_chat_switched));
	session->connect("delta", callable_mp(this, &AIRemoteServer::_on_session_delta));
	session->connect("run_state_changed", callable_mp(this, &AIRemoteServer::_on_session_run_state));
	session->connect("status_changed", callable_mp(this, &AIRemoteServer::_on_session_status));
	session->connect("approval_changed", callable_mp(this, &AIRemoteServer::_on_session_approval));
	session->connect("policy_mode_changed", callable_mp(this, &AIRemoteServer::_on_session_policy_mode));
	session_connected = true;
}

void AIRemoteServer::_disconnect_session() {
	AIChatSession *session = AIChatSession::get_singleton();
	if (!session || !session_connected) {
		session_connected = false;
		return;
	}
	session->disconnect("item_appended", callable_mp(this, &AIRemoteServer::_on_session_item_appended));
	session->disconnect("history_changed", callable_mp(this, &AIRemoteServer::_on_session_history_changed));
	session->disconnect("chat_switched", callable_mp(this, &AIRemoteServer::_on_session_chat_switched));
	session->disconnect("delta", callable_mp(this, &AIRemoteServer::_on_session_delta));
	session->disconnect("run_state_changed", callable_mp(this, &AIRemoteServer::_on_session_run_state));
	session->disconnect("status_changed", callable_mp(this, &AIRemoteServer::_on_session_status));
	session->disconnect("approval_changed", callable_mp(this, &AIRemoteServer::_on_session_approval));
	session->disconnect("policy_mode_changed", callable_mp(this, &AIRemoteServer::_on_session_policy_mode));
	session_connected = false;
}

void AIRemoteServer::_broadcast(const Dictionary &p_msg, bool p_only_controllers) {
	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		Conn *c = &conns[i];
		if (!c->in_use.is_set() || !c->authenticated) {
			continue;
		}
		if (p_only_controllers && !c->can_control) {
			continue;
		}
		_queue_json(c, p_msg);
	}
}

void AIRemoteServer::_on_session_item_appended(int64_t p_ts, const Dictionary &p_data) {
	Dictionary msg;
	msg["t"] = "item";
	msg["ts"] = p_ts;
	msg["item"] = _sanitize_item(p_data);
	_broadcast(msg);
}

void AIRemoteServer::_on_session_history_changed(const String &p_reason) {
	if (p_reason == "append") {
		return; // already sent as an incremental item
	}
	// A rewind or reload invalidates the client's copy; tell it to refetch.
	Dictionary msg;
	msg["t"] = "history_invalidated";
	msg["reason"] = p_reason;
	_broadcast(msg);
}

void AIRemoteServer::_on_session_chat_switched(const String &p_chat_id) {
	Dictionary msg;
	msg["t"] = "chat_switched";
	msg["chat_id"] = p_chat_id;
	_broadcast(msg);
}

void AIRemoteServer::_on_session_delta(const String &p_kind, const String &p_text) {
	Dictionary msg;
	msg["t"] = "delta";
	msg["kind"] = p_kind;
	msg["text"] = p_text;
	_broadcast(msg);
}

void AIRemoteServer::_on_session_run_state(bool p_running) {
	Dictionary msg;
	msg["t"] = "run_state";
	msg["running"] = p_running;
	_broadcast(msg);
}

void AIRemoteServer::_on_session_status(const String &p_text) {
	Dictionary msg;
	msg["t"] = "status";
	msg["text"] = p_text;
	_broadcast(msg);
}

void AIRemoteServer::_on_session_approval(const Dictionary &p_info) {
	Dictionary msg;
	msg["t"] = "approval";
	msg["info"] = p_info.is_empty() ? Variant() : Variant(p_info);
	_broadcast(msg);
}

void AIRemoteServer::_on_session_policy_mode(int p_mode) {
	Dictionary msg;
	msg["t"] = "policy_mode";
	msg["mode"] = p_mode;
	_broadcast(msg);
}

// ---------------------------------------------------------------------------
// Pairing + devices (main thread / UI)
// ---------------------------------------------------------------------------

String AIRemoteServer::begin_pairing() {
	if (!running.is_set()) {
		return String();
	}
	PackedByteArray secret = AIRemoteCrypto::random_bytes(32);
	if (secret.size() != 32) {
		return String();
	}
	{
		MutexLock lock(pairing_mutex);
		pairing_secret = secret;
		pairing_expires_ms = _now_ms() + PAIRING_WINDOW_MS;
		pairing_attempts = 0;
	}
	AIRemoteDeviceStore::audit("pairing_window_opened", Dictionary());
	emit_signal(SNAME("state_changed"));
	return AIRemoteCrypto::b64url_encode(secret);
}

void AIRemoteServer::cancel_pairing() {
	MutexLock lock(pairing_mutex);
	pairing_secret = PackedByteArray();
	pairing_expires_ms = 0;
	pairing_attempts = 0;
}

bool AIRemoteServer::is_pairing_active() {
	MutexLock lock(pairing_mutex);
	return !pairing_secret.is_empty() && _now_ms() <= pairing_expires_ms;
}

int AIRemoteServer::get_pairing_seconds_left() {
	MutexLock lock(pairing_mutex);
	if (pairing_secret.is_empty()) {
		return 0;
	}
	const uint64_t now = _now_ms();
	if (now >= pairing_expires_ms) {
		return 0;
	}
	return (int)((pairing_expires_ms - now) / 1000);
}

Array AIRemoteServer::get_devices() {
	MutexLock lock(devices_mutex);
	if (devices.get_devices().is_empty()) {
		devices.load();
	}
	Array out;
	const Vector<AIRemoteDevice> &list = devices.get_devices();
	for (int i = 0; i < list.size(); i++) {
		out.push_back(list[i].to_dict());
	}
	return out;
}

bool AIRemoteServer::revoke_device(const String &p_id) {
	bool ok = false;
	{
		MutexLock lock(devices_mutex);
		ok = devices.remove_device(p_id);
	}
	if (ok) {
		emit_signal(SNAME("devices_changed"));
	}
	return ok;
}

bool AIRemoteServer::revoke_all_devices() {
	{
		MutexLock lock(devices_mutex);
		devices.remove_all();
	}
	emit_signal(SNAME("devices_changed"));
	return true;
}

bool AIRemoteServer::set_device_control(const String &p_id, bool p_allow) {
	bool ok = false;
	{
		MutexLock lock(devices_mutex);
		ok = devices.set_allow_control(p_id, p_allow);
	}
	if (ok) {
		// Live sessions pick the change up on their next command, but push a
		// fresh state so the client's UI updates immediately.
		for (int i = 0; i < MAX_CONNECTIONS; i++) {
			Conn *c = &conns[i];
			if (c->in_use.is_set() && c->authenticated && c->device_id == p_id) {
				c->can_control = p_allow;
				_queue_json(c, _state_dict(p_allow));
			}
		}
		emit_signal(SNAME("devices_changed"));
	}
	return ok;
}
