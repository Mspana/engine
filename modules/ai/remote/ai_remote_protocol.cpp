/**************************************************************************/
/*  ai_remote_protocol.cpp                                                */
/**************************************************************************/

#include "ai_remote_protocol.h"

#include "ai_remote_crypto.h"

#include <string.h>

const char *AIRemoteProtocol::PAIR_INFO = "aristotle-remote-pair-v1";
const char *AIRemoteProtocol::SESSION_INFO = "aristotle-remote-session-v1";

PackedByteArray AIRemoteProtocol::make_nonce(uint8_t p_direction, uint64_t p_counter) {
	PackedByteArray nonce;
	nonce.resize(AIRemoteCrypto::AEAD_NONCE_SIZE);
	uint8_t *w = nonce.ptrw();
	memset(w, 0, AIRemoteCrypto::AEAD_NONCE_SIZE);
	w[3] = p_direction;
	for (int i = 0; i < 8; i++) {
		w[4 + i] = (uint8_t)((p_counter >> (56 - 8 * i)) & 0xFF);
	}
	return nonce;
}

// ---------------------------------------------------------------------------
// Pairing
// ---------------------------------------------------------------------------

PackedByteArray AIRemoteProtocol::derive_pair_key(const PackedByteArray &p_own_private,
		const PackedByteArray &p_peer_public, const PackedByteArray &p_pairing_secret,
		const PackedByteArray &p_salt) {
	PackedByteArray shared;
	if (!AIRemoteCrypto::x25519(p_own_private, p_peer_public, shared)) {
		return PackedByteArray();
	}
	// Mixing the pairing secret into the IKM is what authenticates this
	// exchange: without it the derived key is unreachable.
	PackedByteArray ikm = shared;
	ikm.append_array(p_pairing_secret);
	return AIRemoteCrypto::hkdf_sha256(ikm, p_salt, PAIR_INFO, AIRemoteCrypto::AEAD_KEY_SIZE);
}

PackedByteArray AIRemoteProtocol::pair_proof_plaintext(const PackedByteArray &p_device_public,
		const PackedByteArray &p_server_public) {
	PackedByteArray out;
	const char *tag = "pair-confirm";
	out.resize((int)strlen(tag));
	memcpy(out.ptrw(), tag, strlen(tag));
	out.append_array(p_device_public);
	out.append_array(p_server_public);
	return out;
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

bool AIRemoteProtocol::derive_session_keys(bool p_is_server,
		const PackedByteArray &p_own_static_private, const PackedByteArray &p_own_ephemeral_private,
		const PackedByteArray &p_peer_static_public, const PackedByteArray &p_peer_ephemeral_public,
		const PackedByteArray &p_nonce_client, const PackedByteArray &p_nonce_server,
		PackedByteArray &r_key_c2s, PackedByteArray &r_key_s2c) {
	PackedByteArray ee, es, se, ss;

	// Both sides must build the same four terms in the same order. The names
	// are from the client's point of view: "es" is client-ephemeral with
	// server-static, so the server computes it from its static key.
	if (!AIRemoteCrypto::x25519(p_own_ephemeral_private, p_peer_ephemeral_public, ee)) {
		return false;
	}
	if (p_is_server) {
		if (!AIRemoteCrypto::x25519(p_own_static_private, p_peer_ephemeral_public, es)) {
			return false;
		}
		if (!AIRemoteCrypto::x25519(p_own_ephemeral_private, p_peer_static_public, se)) {
			return false;
		}
	} else {
		if (!AIRemoteCrypto::x25519(p_own_ephemeral_private, p_peer_static_public, es)) {
			return false;
		}
		if (!AIRemoteCrypto::x25519(p_own_static_private, p_peer_ephemeral_public, se)) {
			return false;
		}
	}
	if (!AIRemoteCrypto::x25519(p_own_static_private, p_peer_static_public, ss)) {
		return false;
	}

	PackedByteArray ikm;
	ikm.append_array(ee);
	ikm.append_array(es);
	ikm.append_array(se);
	ikm.append_array(ss);

	PackedByteArray salt = p_nonce_client;
	salt.append_array(p_nonce_server);

	PackedByteArray okm = AIRemoteCrypto::hkdf_sha256(ikm, salt, SESSION_INFO,
			AIRemoteCrypto::AEAD_KEY_SIZE * 2);
	if (okm.size() != AIRemoteCrypto::AEAD_KEY_SIZE * 2) {
		return false;
	}

	r_key_c2s = okm.slice(0, AIRemoteCrypto::AEAD_KEY_SIZE);
	r_key_s2c = okm.slice(AIRemoteCrypto::AEAD_KEY_SIZE, AIRemoteCrypto::AEAD_KEY_SIZE * 2);
	return true;
}

// ---------------------------------------------------------------------------
// AIRemoteChannel
// ---------------------------------------------------------------------------

void AIRemoteChannel::establish(const PackedByteArray &p_key_send, const PackedByteArray &p_key_recv,
		uint8_t p_dir_send, uint8_t p_dir_recv) {
	key_send = p_key_send;
	key_recv = p_key_recv;
	dir_send = p_dir_send;
	dir_recv = p_dir_recv;
	send_counter = 0;
	last_recv_counter = -1;
	established = key_send.size() == AIRemoteCrypto::AEAD_KEY_SIZE &&
			key_recv.size() == AIRemoteCrypto::AEAD_KEY_SIZE;
}

void AIRemoteChannel::reset() {
	key_send = PackedByteArray();
	key_recv = PackedByteArray();
	send_counter = 0;
	last_recv_counter = -1;
	established = false;
}

PackedByteArray AIRemoteChannel::encrypt(const PackedByteArray &p_plaintext) {
	PackedByteArray out;
	if (!established) {
		return out;
	}

	PackedByteArray header;
	header.resize(AIRemoteProtocol::FRAME_HEADER_SIZE);
	uint8_t *h = header.ptrw();
	h[0] = AIRemoteProtocol::FRAME_VERSION;
	h[1] = AIRemoteProtocol::FRAME_TYPE_DATA;
	for (int i = 0; i < 8; i++) {
		h[2 + i] = (uint8_t)((send_counter >> (56 - 8 * i)) & 0xFF);
	}

	PackedByteArray nonce = AIRemoteProtocol::make_nonce(dir_send, send_counter);
	// The header is authenticated but not encrypted, so a tampered counter is
	// detected rather than silently accepted.
	PackedByteArray ct = AIRemoteCrypto::aes_gcm_encrypt(key_send, nonce, p_plaintext, header);
	if (ct.is_empty() && !p_plaintext.is_empty()) {
		return PackedByteArray();
	}

	send_counter++;
	out = header;
	out.append_array(ct);
	return out;
}

bool AIRemoteChannel::decrypt(const PackedByteArray &p_frame, PackedByteArray &r_plaintext) {
	if (!established) {
		return false;
	}
	if (p_frame.size() < AIRemoteProtocol::FRAME_HEADER_SIZE + AIRemoteCrypto::AEAD_TAG_SIZE) {
		return false;
	}
	const uint8_t *f = p_frame.ptr();
	if (f[0] != AIRemoteProtocol::FRAME_VERSION || f[1] != AIRemoteProtocol::FRAME_TYPE_DATA) {
		return false;
	}

	uint64_t counter = 0;
	for (int i = 0; i < 8; i++) {
		counter = (counter << 8) | (uint64_t)f[2 + i];
	}
	// Strictly increasing: blocks replay and reordering by a hostile relay.
	if (last_recv_counter >= 0 && counter <= (uint64_t)last_recv_counter) {
		return false;
	}

	PackedByteArray header = p_frame.slice(0, AIRemoteProtocol::FRAME_HEADER_SIZE);
	PackedByteArray body = p_frame.slice(AIRemoteProtocol::FRAME_HEADER_SIZE);
	PackedByteArray nonce = AIRemoteProtocol::make_nonce(dir_recv, counter);

	PackedByteArray plain;
	if (!AIRemoteCrypto::aes_gcm_decrypt(key_recv, nonce, body, header, plain)) {
		return false;
	}

	last_recv_counter = (int64_t)counter;
	r_plaintext = plain;
	return true;
}
