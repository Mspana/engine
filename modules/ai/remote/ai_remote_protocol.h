/**************************************************************************/
/*  ai_remote_protocol.h                                                  */
/**************************************************************************/
/* Handshake derivations and the encrypted frame format.                  */
/*                                                                        */
/* The session handshake is Noise-KK shaped: mutual authentication comes  */
/* from the two static keys, forward secrecy from the two ephemerals. The */
/* pairing handshake authenticates by mixing a single-use pairing secret  */
/* into the key material, so a party without the secret — including a     */
/* relay in the middle — cannot derive the key.                           */
/**************************************************************************/

#ifndef AI_REMOTE_PROTOCOL_H
#define AI_REMOTE_PROTOCOL_H

#include "core/string/ustring.h"
#include "core/variant/variant.h"

class AIRemoteProtocol {
public:
	static const uint8_t FRAME_VERSION = 1;
	static const uint8_t FRAME_TYPE_DATA = 1;
	static const int FRAME_HEADER_SIZE = 10; // version, type, 8-byte counter

	// Nonce prefixes keep the three key usages in separate nonce spaces.
	static const uint8_t DIR_PAIR = 1;
	static const uint8_t DIR_C2S = 2;
	static const uint8_t DIR_S2C = 3;

	static const char *PAIR_INFO; // "aristotle-remote-pair-v1"
	static const char *SESSION_INFO; // "aristotle-remote-session-v1"

	static PackedByteArray make_nonce(uint8_t p_direction, uint64_t p_counter);

	// ---- Pairing ------------------------------------------------------------
	// k = HKDF(ikm = X25519(own_priv, peer_pub) || pairing_secret, salt, PAIR_INFO)
	static PackedByteArray derive_pair_key(const PackedByteArray &p_own_private,
			const PackedByteArray &p_peer_public, const PackedByteArray &p_pairing_secret,
			const PackedByteArray &p_salt);
	// "pair-confirm" || device_pub || server_pub
	static PackedByteArray pair_proof_plaintext(const PackedByteArray &p_device_public,
			const PackedByteArray &p_server_public);

	// ---- Session ------------------------------------------------------------
	// Derives both directional keys from the four Diffie-Hellman terms.
	static bool derive_session_keys(bool p_is_server,
			const PackedByteArray &p_own_static_private, const PackedByteArray &p_own_ephemeral_private,
			const PackedByteArray &p_peer_static_public, const PackedByteArray &p_peer_ephemeral_public,
			const PackedByteArray &p_nonce_client, const PackedByteArray &p_nonce_server,
			PackedByteArray &r_key_c2s, PackedByteArray &r_key_s2c);
};

// Per-connection encrypted channel: framing plus replay protection.
class AIRemoteChannel {
	PackedByteArray key_send;
	PackedByteArray key_recv;
	uint8_t dir_send = AIRemoteProtocol::DIR_S2C;
	uint8_t dir_recv = AIRemoteProtocol::DIR_C2S;
	uint64_t send_counter = 0;
	int64_t last_recv_counter = -1;
	bool established = false;

public:
	void establish(const PackedByteArray &p_key_send, const PackedByteArray &p_key_recv,
			uint8_t p_dir_send, uint8_t p_dir_recv);
	void reset();
	bool is_established() const { return established; }

	// Wraps plaintext into a complete wire frame. Empty on failure.
	PackedByteArray encrypt(const PackedByteArray &p_plaintext);
	// Unwraps a wire frame. Fails on bad version/type, authentication failure,
	// or a counter that is not strictly greater than the last accepted one.
	bool decrypt(const PackedByteArray &p_frame, PackedByteArray &r_plaintext);

	// Test/introspection helpers.
	uint64_t get_send_counter() const { return send_counter; }
	int64_t get_last_recv_counter() const { return last_recv_counter; }
};

#endif // AI_REMOTE_PROTOCOL_H
