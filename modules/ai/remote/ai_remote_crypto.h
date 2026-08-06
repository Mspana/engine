/**************************************************************************/
/*  ai_remote_crypto.h                                                    */
/**************************************************************************/
/* Primitives for the remote-access channel, chosen so a browser can      */
/* speak the same protocol using only the built-in WebCrypto API (no      */
/* third-party JavaScript): X25519 + HKDF-SHA256 + AES-256-GCM.           */
/*                                                                        */
/* Backed by mbedTLS, which modules/mbedtls already compiles into the     */
/* binary. This file must be built with the same MBEDTLS_CONFIG_FILE the  */
/* library was built with (see modules/ai/SCsub).                         */
/**************************************************************************/

#ifndef AI_REMOTE_CRYPTO_H
#define AI_REMOTE_CRYPTO_H

#include "core/string/ustring.h"
#include "core/variant/variant.h"

class AIRemoteCrypto {
public:
	static const int X25519_KEY_SIZE = 32;
	static const int AEAD_KEY_SIZE = 32;
	static const int AEAD_NONCE_SIZE = 12;
	static const int AEAD_TAG_SIZE = 16;

	// Cryptographically secure random bytes. Returns empty on failure.
	static PackedByteArray random_bytes(int p_count);

	// X25519. generate_keypair fills both; both outputs are 32 bytes.
	static bool generate_keypair(PackedByteArray &r_private, PackedByteArray &r_public);
	static bool public_from_private(const PackedByteArray &p_private, PackedByteArray &r_public);
	// Raw X25519 shared secret. Rejects all-zero results (low-order peer keys).
	static bool x25519(const PackedByteArray &p_private, const PackedByteArray &p_peer_public, PackedByteArray &r_shared);

	// HKDF-SHA256 extract+expand.
	static PackedByteArray hkdf_sha256(const PackedByteArray &p_ikm, const PackedByteArray &p_salt,
			const String &p_info, int p_length);

	// AES-256-GCM. The tag is appended to the ciphertext, matching WebCrypto.
	static PackedByteArray aes_gcm_encrypt(const PackedByteArray &p_key, const PackedByteArray &p_nonce,
			const PackedByteArray &p_plaintext, const PackedByteArray &p_aad);
	// Returns false on any authentication failure. r_plaintext is untouched then.
	static bool aes_gcm_decrypt(const PackedByteArray &p_key, const PackedByteArray &p_nonce,
			const PackedByteArray &p_ciphertext, const PackedByteArray &p_aad, PackedByteArray &r_plaintext);

	// Constant-time comparison — never use == on secrets.
	static bool const_time_equals(const PackedByteArray &p_a, const PackedByteArray &p_b);

	// Base64 helpers (standard alphabet, padded) for JSON transport.
	static String b64_encode(const PackedByteArray &p_data);
	static PackedByteArray b64_decode(const String &p_b64);
	// URL-safe, unpadded — used for pairing codes so they survive typing/QR.
	static String b64url_encode(const PackedByteArray &p_data);
	static PackedByteArray b64url_decode(const String &p_b64);

	// Short human-comparable fingerprint of a public key (first 8 bytes, hex).
	static String fingerprint(const PackedByteArray &p_public);
};

#endif // AI_REMOTE_CRYPTO_H
