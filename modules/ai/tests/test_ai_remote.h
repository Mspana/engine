// modules/ai/tests/test_ai_remote.h
//
// Covers the remote-access crypto, framing and replay protection. These run
// entirely in-process: no sockets, no editor, no network.

#pragma once

#include "../remote/ai_remote_crypto.h"
#include "../remote/ai_remote_protocol.h"
#include "../remote/ai_remote_server.h"
#include "../remote/ai_remote_ws.h"

#include "tests/test_macros.h"

namespace TestAIRemote {

static PackedByteArray _bytes(const char *p_str) {
	PackedByteArray out;
	const int len = (int)strlen(p_str);
	out.resize(len);
	for (int i = 0; i < len; i++) {
		out.write[i] = (uint8_t)p_str[i];
	}
	return out;
}

// Client frames must be masked, which build_frame (a server helper) never does.
static PackedByteArray _mask_client_frame(int p_opcode, const PackedByteArray &p_payload, bool p_fin = true) {
	PackedByteArray out;
	const int len = p_payload.size();
	out.push_back((p_fin ? 0x80 : 0x00) | (uint8_t)p_opcode);
	if (len < 126) {
		out.push_back(0x80 | (uint8_t)len);
	} else if (len <= 0xFFFF) {
		out.push_back(0x80 | 126);
		out.push_back((uint8_t)((len >> 8) & 0xFF));
		out.push_back((uint8_t)(len & 0xFF));
	} else {
		out.push_back(0x80 | 127);
		for (int i = 0; i < 8; i++) {
			out.push_back((uint8_t)(((uint64_t)len >> (56 - 8 * i)) & 0xFF));
		}
	}
	const uint8_t mask[4] = { 0xA1, 0xB2, 0xC3, 0xD4 };
	for (int i = 0; i < 4; i++) {
		out.push_back(mask[i]);
	}
	for (int i = 0; i < len; i++) {
		out.push_back(p_payload[i] ^ mask[i & 3]);
	}
	return out;
}

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------

TEST_CASE("[AIRemote] X25519 produces an identical shared secret on both sides") {
	PackedByteArray a_priv, a_pub, b_priv, b_pub;
	REQUIRE(AIRemoteCrypto::generate_keypair(a_priv, a_pub));
	REQUIRE(AIRemoteCrypto::generate_keypair(b_priv, b_pub));
	CHECK(a_pub.size() == 32);
	CHECK(a_pub != b_pub);

	PackedByteArray s1, s2;
	REQUIRE(AIRemoteCrypto::x25519(a_priv, b_pub, s1));
	REQUIRE(AIRemoteCrypto::x25519(b_priv, a_pub, s2));
	CHECK(s1.size() == 32);
	CHECK(AIRemoteCrypto::const_time_equals(s1, s2));

	// A third party's key must not reproduce that secret.
	PackedByteArray c_priv, c_pub, s3;
	REQUIRE(AIRemoteCrypto::generate_keypair(c_priv, c_pub));
	REQUIRE(AIRemoteCrypto::x25519(c_priv, a_pub, s3));
	CHECK_FALSE(AIRemoteCrypto::const_time_equals(s1, s3));
}

TEST_CASE("[AIRemote] X25519 rejects an all-zero (low-order) peer key") {
	PackedByteArray priv, pub;
	REQUIRE(AIRemoteCrypto::generate_keypair(priv, pub));

	PackedByteArray zero;
	zero.resize(32);
	memset(zero.ptrw(), 0, 32);

	PackedByteArray shared;
	ERR_PRINT_OFF;
	CHECK_FALSE(AIRemoteCrypto::x25519(priv, zero, shared));
	ERR_PRINT_ON;
}

TEST_CASE("[AIRemote] HKDF is deterministic and salt-sensitive") {
	PackedByteArray ikm = _bytes("input-key-material");
	PackedByteArray salt = _bytes("salt-value");

	PackedByteArray k1 = AIRemoteCrypto::hkdf_sha256(ikm, salt, "info-a", 64);
	PackedByteArray k2 = AIRemoteCrypto::hkdf_sha256(ikm, salt, "info-a", 64);
	CHECK(k1.size() == 64);
	CHECK(AIRemoteCrypto::const_time_equals(k1, k2));

	CHECK_FALSE(AIRemoteCrypto::const_time_equals(k1, AIRemoteCrypto::hkdf_sha256(ikm, salt, "info-b", 64)));
	CHECK_FALSE(AIRemoteCrypto::const_time_equals(k1, AIRemoteCrypto::hkdf_sha256(ikm, _bytes("other"), "info-a", 64)));
}

TEST_CASE("[AIRemote] AES-GCM round-trips and detects tampering") {
	PackedByteArray key = AIRemoteCrypto::random_bytes(32);
	PackedByteArray nonce = AIRemoteProtocol::make_nonce(AIRemoteProtocol::DIR_C2S, 7);
	PackedByteArray plain = _bytes("{\"t\":\"send\",\"text\":\"hello world\"}");
	PackedByteArray aad = _bytes("header-bytes");

	PackedByteArray ct = AIRemoteCrypto::aes_gcm_encrypt(key, nonce, plain, aad);
	CHECK(ct.size() == plain.size() + 16);

	PackedByteArray out;
	REQUIRE(AIRemoteCrypto::aes_gcm_decrypt(key, nonce, ct, aad, out));
	CHECK(out == plain);

	// Flipped ciphertext bit.
	PackedByteArray bad = ct;
	bad.write[0] ^= 0x01;
	CHECK_FALSE(AIRemoteCrypto::aes_gcm_decrypt(key, nonce, bad, aad, out));

	// Wrong associated data (a tampered frame header).
	CHECK_FALSE(AIRemoteCrypto::aes_gcm_decrypt(key, nonce, ct, _bytes("other-header"), out));

	// Wrong nonce (a replayed counter with a different value).
	PackedByteArray other_nonce = AIRemoteProtocol::make_nonce(AIRemoteProtocol::DIR_C2S, 8);
	CHECK_FALSE(AIRemoteCrypto::aes_gcm_decrypt(key, other_nonce, ct, aad, out));

	// Wrong key.
	CHECK_FALSE(AIRemoteCrypto::aes_gcm_decrypt(AIRemoteCrypto::random_bytes(32), nonce, ct, aad, out));
}

TEST_CASE("[AIRemote] Base64 helpers round-trip, including the URL-safe form") {
	PackedByteArray data = AIRemoteCrypto::random_bytes(32);
	CHECK(AIRemoteCrypto::b64_decode(AIRemoteCrypto::b64_encode(data)) == data);

	const String url = AIRemoteCrypto::b64url_encode(data);
	CHECK_FALSE(url.contains("+"));
	CHECK_FALSE(url.contains("/"));
	CHECK_FALSE(url.contains("="));
	CHECK(AIRemoteCrypto::b64url_decode(url) == data);
}

TEST_CASE("[AIRemote] Constant-time compare matches only on identical input") {
	PackedByteArray a = _bytes("abcdef");
	CHECK(AIRemoteCrypto::const_time_equals(a, _bytes("abcdef")));
	CHECK_FALSE(AIRemoteCrypto::const_time_equals(a, _bytes("abcdeg")));
	CHECK_FALSE(AIRemoteCrypto::const_time_equals(a, _bytes("abcde")));
}

// ---------------------------------------------------------------------------
// Handshakes
// ---------------------------------------------------------------------------

TEST_CASE("[AIRemote] Both sides of the session handshake derive the same keys") {
	PackedByteArray srv_priv, srv_pub, dev_priv, dev_pub;
	PackedByteArray eph_c_priv, eph_c_pub, eph_s_priv, eph_s_pub;
	REQUIRE(AIRemoteCrypto::generate_keypair(srv_priv, srv_pub));
	REQUIRE(AIRemoteCrypto::generate_keypair(dev_priv, dev_pub));
	REQUIRE(AIRemoteCrypto::generate_keypair(eph_c_priv, eph_c_pub));
	REQUIRE(AIRemoteCrypto::generate_keypair(eph_s_priv, eph_s_pub));

	PackedByteArray nonce_c = AIRemoteCrypto::random_bytes(32);
	PackedByteArray nonce_s = AIRemoteCrypto::random_bytes(32);

	PackedByteArray c2s_client, s2c_client, c2s_server, s2c_server;
	REQUIRE(AIRemoteProtocol::derive_session_keys(false, dev_priv, eph_c_priv, srv_pub, eph_s_pub,
			nonce_c, nonce_s, c2s_client, s2c_client));
	REQUIRE(AIRemoteProtocol::derive_session_keys(true, srv_priv, eph_s_priv, dev_pub, eph_c_pub,
			nonce_c, nonce_s, c2s_server, s2c_server));

	CHECK(AIRemoteCrypto::const_time_equals(c2s_client, c2s_server));
	CHECK(AIRemoteCrypto::const_time_equals(s2c_client, s2c_server));
	// The two directions must not share a key.
	CHECK_FALSE(AIRemoteCrypto::const_time_equals(c2s_client, s2c_client));
}

TEST_CASE("[AIRemote] An impostor device cannot derive the session keys") {
	PackedByteArray srv_priv, srv_pub, dev_priv, dev_pub, bad_priv, bad_pub;
	PackedByteArray eph_c_priv, eph_c_pub, eph_s_priv, eph_s_pub;
	REQUIRE(AIRemoteCrypto::generate_keypair(srv_priv, srv_pub));
	REQUIRE(AIRemoteCrypto::generate_keypair(dev_priv, dev_pub));
	REQUIRE(AIRemoteCrypto::generate_keypair(bad_priv, bad_pub));
	REQUIRE(AIRemoteCrypto::generate_keypair(eph_c_priv, eph_c_pub));
	REQUIRE(AIRemoteCrypto::generate_keypair(eph_s_priv, eph_s_pub));

	PackedByteArray nonce_c = AIRemoteCrypto::random_bytes(32);
	PackedByteArray nonce_s = AIRemoteCrypto::random_bytes(32);

	// The server believes it is talking to dev_pub; the attacker holds bad_priv.
	PackedByteArray c2s_server, s2c_server, c2s_attacker, s2c_attacker;
	REQUIRE(AIRemoteProtocol::derive_session_keys(true, srv_priv, eph_s_priv, dev_pub, eph_c_pub,
			nonce_c, nonce_s, c2s_server, s2c_server));
	REQUIRE(AIRemoteProtocol::derive_session_keys(false, bad_priv, eph_c_priv, srv_pub, eph_s_pub,
			nonce_c, nonce_s, c2s_attacker, s2c_attacker));

	CHECK_FALSE(AIRemoteCrypto::const_time_equals(c2s_server, c2s_attacker));
}

TEST_CASE("[AIRemote] Pairing proof verifies only with the correct pairing secret") {
	PackedByteArray srv_priv, srv_pub, dev_priv, dev_pub;
	REQUIRE(AIRemoteCrypto::generate_keypair(srv_priv, srv_pub));
	REQUIRE(AIRemoteCrypto::generate_keypair(dev_priv, dev_pub));

	PackedByteArray secret = AIRemoteCrypto::random_bytes(32);
	PackedByteArray salt = AIRemoteCrypto::random_bytes(32);

	PackedByteArray k_client = AIRemoteProtocol::derive_pair_key(dev_priv, srv_pub, secret, salt);
	PackedByteArray k_server = AIRemoteProtocol::derive_pair_key(srv_priv, dev_pub, secret, salt);
	REQUIRE(k_client.size() == 32);
	CHECK(AIRemoteCrypto::const_time_equals(k_client, k_server));

	const PackedByteArray expected = AIRemoteProtocol::pair_proof_plaintext(dev_pub, srv_pub);
	const PackedByteArray nonce = AIRemoteProtocol::make_nonce(AIRemoteProtocol::DIR_PAIR, 0);
	PackedByteArray proof = AIRemoteCrypto::aes_gcm_encrypt(k_client, nonce, expected, PackedByteArray());

	PackedByteArray got;
	REQUIRE(AIRemoteCrypto::aes_gcm_decrypt(k_server, nonce, proof, PackedByteArray(), got));
	CHECK(AIRemoteCrypto::const_time_equals(got, expected));

	// Same key exchange, wrong pairing code: the server's key no longer matches,
	// which is exactly what stops a machine-in-the-middle.
	PackedByteArray wrong = AIRemoteProtocol::derive_pair_key(srv_priv, dev_pub,
			AIRemoteCrypto::random_bytes(32), salt);
	CHECK_FALSE(AIRemoteCrypto::aes_gcm_decrypt(wrong, nonce, proof, PackedByteArray(), got));
}

// ---------------------------------------------------------------------------
// Channel framing and replay protection
// ---------------------------------------------------------------------------

TEST_CASE("[AIRemote] Channel round-trips messages between the two directions") {
	PackedByteArray k_c2s = AIRemoteCrypto::random_bytes(32);
	PackedByteArray k_s2c = AIRemoteCrypto::random_bytes(32);

	AIRemoteChannel server, client;
	server.establish(k_s2c, k_c2s, AIRemoteProtocol::DIR_S2C, AIRemoteProtocol::DIR_C2S);
	client.establish(k_c2s, k_s2c, AIRemoteProtocol::DIR_C2S, AIRemoteProtocol::DIR_S2C);
	CHECK(server.is_established());

	PackedByteArray msg = _bytes("{\"t\":\"ping\"}");
	PackedByteArray frame = client.encrypt(msg);
	CHECK(frame.size() == AIRemoteProtocol::FRAME_HEADER_SIZE + msg.size() + 16);

	PackedByteArray out;
	REQUIRE(server.decrypt(frame, out));
	CHECK(out == msg);

	// And back the other way.
	PackedByteArray reply = _bytes("{\"t\":\"pong\"}");
	PackedByteArray reply_frame = server.encrypt(reply);
	REQUIRE(client.decrypt(reply_frame, out));
	CHECK(out == reply);
}

TEST_CASE("[AIRemote] Channel rejects replayed, stale and tampered frames") {
	PackedByteArray k_c2s = AIRemoteCrypto::random_bytes(32);
	PackedByteArray k_s2c = AIRemoteCrypto::random_bytes(32);

	AIRemoteChannel server, client;
	server.establish(k_s2c, k_c2s, AIRemoteProtocol::DIR_S2C, AIRemoteProtocol::DIR_C2S);
	client.establish(k_c2s, k_s2c, AIRemoteProtocol::DIR_C2S, AIRemoteProtocol::DIR_S2C);

	PackedByteArray f0 = client.encrypt(_bytes("first"));
	PackedByteArray f1 = client.encrypt(_bytes("second"));
	PackedByteArray f2 = client.encrypt(_bytes("third"));

	PackedByteArray out;
	REQUIRE(server.decrypt(f0, out));
	REQUIRE(server.decrypt(f1, out));

	// Replaying an already-seen frame must fail.
	CHECK_FALSE(server.decrypt(f1, out));
	CHECK_FALSE(server.decrypt(f0, out));

	// A newer frame still works, and skipping ahead is allowed (a dropped
	// frame must not wedge the session) — going backwards is not.
	REQUIRE(server.decrypt(f2, out));
	CHECK(out == _bytes("third"));

	// Tampering with the authenticated header is detected.
	PackedByteArray f3 = client.encrypt(_bytes("fourth"));
	f3.write[9] ^= 0x01; // last counter byte
	CHECK_FALSE(server.decrypt(f3, out));

	// Tampering with the ciphertext is detected.
	PackedByteArray f4 = client.encrypt(_bytes("fifth"));
	f4.write[f4.size() - 1] ^= 0x01;
	CHECK_FALSE(server.decrypt(f4, out));
}

TEST_CASE("[AIRemote] Channel refuses to operate before it is established") {
	AIRemoteChannel channel;
	CHECK_FALSE(channel.is_established());
	CHECK(channel.encrypt(_bytes("nope")).is_empty());

	PackedByteArray out;
	CHECK_FALSE(channel.decrypt(_bytes("garbage-frame-bytes-here-padding"), out));
}

// ---------------------------------------------------------------------------
// WebSocket framing
// ---------------------------------------------------------------------------

TEST_CASE("[AIRemote] WebSocket accept key matches the RFC 6455 example") {
	CHECK(AIRemoteWS::compute_accept_key("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST_CASE("[AIRemote] Masked client frames parse back to their payload") {
	PackedByteArray payload = _bytes("{\"t\":\"hello\"}");
	PackedByteArray wire = _mask_client_frame(AIRemoteWS::OP_TEXT, payload);

	AIRemoteWS::Frame frame;
	REQUIRE(AIRemoteWS::parse_frame(wire, 0, frame) == AIRemoteWS::PARSE_OK);
	CHECK(frame.fin);
	CHECK(frame.opcode == AIRemoteWS::OP_TEXT);
	CHECK(frame.payload == payload);
	CHECK(frame.consumed == wire.size());
}

TEST_CASE("[AIRemote] Extended payload lengths parse correctly") {
	// 16-bit length path.
	PackedByteArray medium;
	medium.resize(500);
	for (int i = 0; i < 500; i++) {
		medium.write[i] = (uint8_t)(i & 0xFF);
	}
	PackedByteArray wire = _mask_client_frame(AIRemoteWS::OP_BINARY, medium);
	AIRemoteWS::Frame frame;
	REQUIRE(AIRemoteWS::parse_frame(wire, 0, frame) == AIRemoteWS::PARSE_OK);
	CHECK(frame.payload == medium);

	// 64-bit length path.
	PackedByteArray large;
	large.resize(70000);
	memset(large.ptrw(), 0x5A, 70000);
	PackedByteArray wire2 = _mask_client_frame(AIRemoteWS::OP_BINARY, large);
	AIRemoteWS::Frame frame2;
	REQUIRE(AIRemoteWS::parse_frame(wire2, 0, frame2) == AIRemoteWS::PARSE_OK);
	CHECK(frame2.payload.size() == 70000);
}

TEST_CASE("[AIRemote] Incomplete input is reported rather than misparsed") {
	PackedByteArray wire = _mask_client_frame(AIRemoteWS::OP_TEXT, _bytes("some payload here"));
	AIRemoteWS::Frame frame;
	for (int cut = 1; cut < wire.size(); cut++) {
		CHECK(AIRemoteWS::parse_frame(wire.slice(0, cut), 0, frame) == AIRemoteWS::PARSE_INCOMPLETE);
	}
	CHECK(AIRemoteWS::parse_frame(wire, 0, frame) == AIRemoteWS::PARSE_OK);
}

TEST_CASE("[AIRemote] Unmasked client frames are a protocol violation") {
	// build_frame is the server helper, so its output is unmasked by design.
	PackedByteArray wire = AIRemoteWS::build_frame(AIRemoteWS::OP_TEXT, _bytes("unmasked"));
	AIRemoteWS::Frame frame;
	CHECK(AIRemoteWS::parse_frame(wire, 0, frame) == AIRemoteWS::PARSE_ERROR);
}

TEST_CASE("[AIRemote] Reserved bits and oversized control frames are rejected") {
	PackedByteArray wire = _mask_client_frame(AIRemoteWS::OP_TEXT, _bytes("hi"));
	wire.write[0] |= 0x40; // set RSV1
	AIRemoteWS::Frame frame;
	CHECK(AIRemoteWS::parse_frame(wire, 0, frame) == AIRemoteWS::PARSE_ERROR);

	PackedByteArray big_ping;
	big_ping.resize(200);
	memset(big_ping.ptrw(), 'x', 200);
	PackedByteArray wire2 = _mask_client_frame(AIRemoteWS::OP_PING, big_ping);
	CHECK(AIRemoteWS::parse_frame(wire2, 0, frame) == AIRemoteWS::PARSE_ERROR);
}

TEST_CASE("[AIRemote] Fragmented messages reassemble in order") {
	AIRemoteWS::MessageAssembler assembler;
	PackedByteArray out;
	int opcode = 0;
	bool error = false;

	AIRemoteWS::Frame f1;
	f1.fin = false;
	f1.opcode = AIRemoteWS::OP_TEXT;
	f1.payload = _bytes("Hello, ");
	CHECK_FALSE(assembler.feed(f1, out, opcode, error));
	CHECK_FALSE(error);

	AIRemoteWS::Frame f2;
	f2.fin = false;
	f2.opcode = AIRemoteWS::OP_CONTINUATION;
	f2.payload = _bytes("remote ");
	CHECK_FALSE(assembler.feed(f2, out, opcode, error));

	AIRemoteWS::Frame f3;
	f3.fin = true;
	f3.opcode = AIRemoteWS::OP_CONTINUATION;
	f3.payload = _bytes("world");
	REQUIRE(assembler.feed(f3, out, opcode, error));
	CHECK_FALSE(error);
	CHECK(opcode == AIRemoteWS::OP_TEXT);
	CHECK(out == _bytes("Hello, remote world"));
}

TEST_CASE("[AIRemote] A control frame may interleave a fragmented message") {
	AIRemoteWS::MessageAssembler assembler;
	PackedByteArray out;
	int opcode = 0;
	bool error = false;

	AIRemoteWS::Frame start;
	start.fin = false;
	start.opcode = AIRemoteWS::OP_BINARY;
	start.payload = _bytes("part-one");
	CHECK_FALSE(assembler.feed(start, out, opcode, error));

	AIRemoteWS::Frame ping;
	ping.fin = true;
	ping.opcode = AIRemoteWS::OP_PING;
	ping.payload = _bytes("beat");
	REQUIRE(assembler.feed(ping, out, opcode, error));
	CHECK(opcode == AIRemoteWS::OP_PING);
	CHECK_FALSE(error);

	AIRemoteWS::Frame finish;
	finish.fin = true;
	finish.opcode = AIRemoteWS::OP_CONTINUATION;
	finish.payload = _bytes("-part-two");
	REQUIRE(assembler.feed(finish, out, opcode, error));
	CHECK(out == _bytes("part-one-part-two"));
}

// ---------------------------------------------------------------------------
// LAN address ranking
// ---------------------------------------------------------------------------

TEST_CASE("[AIRemote] LAN ranking prefers a real interface over a VPN") {
	// Reproduces the developer machine that motivated this: a NordVPN adapter
	// holding a 10.x address alongside the actual LAN address. Taking the first
	// private address found would advertise an address the phone cannot reach.
	const int lan = AIRemoteServer::score_lan_candidate("192.168.0.88", "Ethernet 5");
	const int vpn = AIRemoteServer::score_lan_candidate("10.5.0.2", "NordLynx");
	CHECK(lan > 0);
	CHECK(lan > vpn);
}

TEST_CASE("[AIRemote] LAN ranking rejects unreachable address classes") {
	CHECK(AIRemoteServer::score_lan_candidate("127.0.0.1", "Loopback") < 0);
	CHECK(AIRemoteServer::score_lan_candidate("169.254.193.72", "Wi-Fi") < 0); // link-local
	CHECK(AIRemoteServer::score_lan_candidate("8.8.8.8", "Ethernet") < 0); // public
	CHECK(AIRemoteServer::score_lan_candidate("not-an-address", "Ethernet") < 0);
	CHECK(AIRemoteServer::score_lan_candidate("192.168.1.999", "Ethernet") < 0);
}

TEST_CASE("[AIRemote] LAN ranking orders the private ranges by likelihood") {
	const int home = AIRemoteServer::score_lan_candidate("192.168.1.10", "Ethernet");
	const int mid = AIRemoteServer::score_lan_candidate("172.20.1.10", "Ethernet");
	const int ten = AIRemoteServer::score_lan_candidate("10.0.1.10", "Ethernet");
	CHECK(home > mid);
	CHECK(mid > ten);
	CHECK(ten > 0);
}

TEST_CASE("[AIRemote] Virtual adapters rank below real ones on equal addresses") {
	const int real = AIRemoteServer::score_lan_candidate("192.168.1.10", "Intel Ethernet Connection");
	CHECK(AIRemoteServer::score_lan_candidate("192.168.1.11", "vEthernet (WSL)") < real);
	CHECK(AIRemoteServer::score_lan_candidate("192.168.1.12", "VMware Network Adapter") < real);
	CHECK(AIRemoteServer::score_lan_candidate("192.168.1.13", "Docker Bridge") < real);
	CHECK(AIRemoteServer::score_lan_candidate("192.168.1.14", "OpenVPN TAP-Windows") < real);
}

TEST_CASE("[AIRemote] A continuation without a start frame is an error") {
	AIRemoteWS::MessageAssembler assembler;
	PackedByteArray out;
	int opcode = 0;
	bool error = false;

	AIRemoteWS::Frame orphan;
	orphan.fin = true;
	orphan.opcode = AIRemoteWS::OP_CONTINUATION;
	orphan.payload = _bytes("dangling");
	CHECK_FALSE(assembler.feed(orphan, out, opcode, error));
	CHECK(error);
}

} // namespace TestAIRemote
