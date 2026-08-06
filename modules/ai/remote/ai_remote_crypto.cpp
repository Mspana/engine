/**************************************************************************/
/*  ai_remote_crypto.cpp                                                  */
/**************************************************************************/

#include "ai_remote_crypto.h"

#include "core/crypto/crypto_core.h"

#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/gcm.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>

#include <string.h>

// ---------------------------------------------------------------------------
// Random
// ---------------------------------------------------------------------------

PackedByteArray AIRemoteCrypto::random_bytes(int p_count) {
	PackedByteArray out;
	ERR_FAIL_COND_V(p_count <= 0, out);
	out.resize(p_count);

	CryptoCore::RandomGenerator rng;
	if (rng.init() != OK) {
		ERR_PRINT("AIRemoteCrypto: RNG init failed.");
		return PackedByteArray();
	}
	if (rng.get_random_bytes(out.ptrw(), (size_t)p_count) != OK) {
		ERR_PRINT("AIRemoteCrypto: RNG draw failed.");
		return PackedByteArray();
	}
	return out;
}

// mbedTLS RNG callback wired to the same CSPRNG.
static int _ai_rng_callback(void *p_ctx, unsigned char *p_buf, size_t p_len) {
	CryptoCore::RandomGenerator *rng = static_cast<CryptoCore::RandomGenerator *>(p_ctx);
	return rng->get_random_bytes(p_buf, p_len) == OK ? 0 : -1;
}

// ---------------------------------------------------------------------------
// X25519
//
// Implemented against mbedtls_ecp rather than the ECDH wrapper so both sides
// stay in raw little-endian byte form, which is what WebCrypto exports.
// ---------------------------------------------------------------------------

bool AIRemoteCrypto::generate_keypair(PackedByteArray &r_private, PackedByteArray &r_public) {
	r_private = random_bytes(X25519_KEY_SIZE);
	if (r_private.size() != X25519_KEY_SIZE) {
		return false;
	}
	// Standard X25519 clamping (RFC 7748 §5): clear the low three bits, clear
	// the top bit, set the second-highest. mbedTLS validates the scalar inside
	// ecp_mul and rejects unclamped keys outright, and WebCrypto clamps the
	// same way, so doing it at generation keeps both sides consistent.
	uint8_t *k = r_private.ptrw();
	k[0] &= 248;
	k[31] &= 127;
	k[31] |= 64;

	return public_from_private(r_private, r_public);
}

bool AIRemoteCrypto::public_from_private(const PackedByteArray &p_private, PackedByteArray &r_public) {
	ERR_FAIL_COND_V(p_private.size() != X25519_KEY_SIZE, false);

	// The X25519 base point is u = 9.
	PackedByteArray base;
	base.resize(X25519_KEY_SIZE);
	memset(base.ptrw(), 0, X25519_KEY_SIZE);
	base.write[0] = 9;

	// Deliberately not the low-order rejection path: multiplying the base point
	// never yields zero, and x25519() would reject a legitimate key.
	mbedtls_ecp_group grp;
	mbedtls_mpi scalar, base_u, result_u;
	mbedtls_ecp_group_init(&grp);
	mbedtls_mpi_init(&scalar);
	mbedtls_mpi_init(&base_u);
	mbedtls_mpi_init(&result_u);

	bool ok = false;
	CryptoCore::RandomGenerator rng;
	do {
		if (rng.init() != OK) {
			break;
		}
		if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) != 0) {
			break;
		}
		if (mbedtls_mpi_read_binary_le(&scalar, (const unsigned char *)p_private.ptr(), X25519_KEY_SIZE) != 0) {
			break;
		}
		if (mbedtls_mpi_read_binary_le(&base_u, (const unsigned char *)base.ptr(), X25519_KEY_SIZE) != 0) {
			break;
		}

		mbedtls_ecp_point base_point, result_point;
		mbedtls_ecp_point_init(&base_point);
		mbedtls_ecp_point_init(&result_point);
		bool inner = false;
		do {
			if (mbedtls_mpi_copy(&base_point.MBEDTLS_PRIVATE(X), &base_u) != 0) {
				break;
			}
			if (mbedtls_mpi_lset(&base_point.MBEDTLS_PRIVATE(Z), 1) != 0) {
				break;
			}
			if (mbedtls_ecp_mul(&grp, &result_point, &scalar, &base_point, _ai_rng_callback, &rng) != 0) {
				break;
			}
			r_public.resize(X25519_KEY_SIZE);
			if (mbedtls_mpi_write_binary_le(&result_point.MBEDTLS_PRIVATE(X),
						(unsigned char *)r_public.ptrw(), X25519_KEY_SIZE) != 0) {
				break;
			}
			inner = true;
		} while (false);
		mbedtls_ecp_point_free(&base_point);
		mbedtls_ecp_point_free(&result_point);
		ok = inner;
	} while (false);

	mbedtls_mpi_free(&scalar);
	mbedtls_mpi_free(&base_u);
	mbedtls_mpi_free(&result_u);
	mbedtls_ecp_group_free(&grp);
	return ok;
}

bool AIRemoteCrypto::x25519(const PackedByteArray &p_private, const PackedByteArray &p_peer_public,
		PackedByteArray &r_shared) {
	ERR_FAIL_COND_V(p_private.size() != X25519_KEY_SIZE, false);
	ERR_FAIL_COND_V(p_peer_public.size() != X25519_KEY_SIZE, false);

	mbedtls_ecp_group grp;
	mbedtls_mpi scalar, peer_u;
	mbedtls_ecp_group_init(&grp);
	mbedtls_mpi_init(&scalar);
	mbedtls_mpi_init(&peer_u);

	bool ok = false;
	CryptoCore::RandomGenerator rng;
	do {
		if (rng.init() != OK) {
			break;
		}
		if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) != 0) {
			break;
		}
		if (mbedtls_mpi_read_binary_le(&scalar, (const unsigned char *)p_private.ptr(), X25519_KEY_SIZE) != 0) {
			break;
		}
		if (mbedtls_mpi_read_binary_le(&peer_u, (const unsigned char *)p_peer_public.ptr(), X25519_KEY_SIZE) != 0) {
			break;
		}

		mbedtls_ecp_point peer_point, result_point;
		mbedtls_ecp_point_init(&peer_point);
		mbedtls_ecp_point_init(&result_point);
		bool inner = false;
		do {
			if (mbedtls_mpi_copy(&peer_point.MBEDTLS_PRIVATE(X), &peer_u) != 0) {
				break;
			}
			if (mbedtls_mpi_lset(&peer_point.MBEDTLS_PRIVATE(Z), 1) != 0) {
				break;
			}
			if (mbedtls_ecp_mul(&grp, &result_point, &scalar, &peer_point, _ai_rng_callback, &rng) != 0) {
				break;
			}
			r_shared.resize(X25519_KEY_SIZE);
			if (mbedtls_mpi_write_binary_le(&result_point.MBEDTLS_PRIVATE(X),
						(unsigned char *)r_shared.ptrw(), X25519_KEY_SIZE) != 0) {
				break;
			}
			inner = true;
		} while (false);
		mbedtls_ecp_point_free(&peer_point);
		mbedtls_ecp_point_free(&result_point);
		ok = inner;
	} while (false);

	mbedtls_mpi_free(&scalar);
	mbedtls_mpi_free(&peer_u);
	mbedtls_ecp_group_free(&grp);

	if (!ok) {
		return false;
	}

	// Contributory-behaviour check: an all-zero shared secret means the peer
	// sent a low-order point trying to force a known key. Fail closed.
	uint8_t acc = 0;
	for (int i = 0; i < X25519_KEY_SIZE; i++) {
		acc |= r_shared[i];
	}
	if (acc == 0) {
		r_shared = PackedByteArray();
		ERR_PRINT("AIRemoteCrypto: rejected low-order peer public key.");
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// HKDF-SHA256
// ---------------------------------------------------------------------------

PackedByteArray AIRemoteCrypto::hkdf_sha256(const PackedByteArray &p_ikm, const PackedByteArray &p_salt,
		const String &p_info, int p_length) {
	PackedByteArray out;
	ERR_FAIL_COND_V(p_length <= 0 || p_length > 255 * 32, out);
	out.resize(p_length);

	const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
	if (!md) {
		ERR_PRINT("AIRemoteCrypto: SHA-256 unavailable.");
		return PackedByteArray();
	}

	CharString info = p_info.utf8();
	int ret = mbedtls_hkdf(md,
			(const unsigned char *)p_salt.ptr(), (size_t)p_salt.size(),
			(const unsigned char *)p_ikm.ptr(), (size_t)p_ikm.size(),
			(const unsigned char *)info.get_data(), (size_t)info.length(),
			(unsigned char *)out.ptrw(), (size_t)p_length);
	if (ret != 0) {
		ERR_PRINT(vformat("AIRemoteCrypto: HKDF failed (%d).", ret));
		return PackedByteArray();
	}
	return out;
}

// ---------------------------------------------------------------------------
// AES-256-GCM
// ---------------------------------------------------------------------------

PackedByteArray AIRemoteCrypto::aes_gcm_encrypt(const PackedByteArray &p_key, const PackedByteArray &p_nonce,
		const PackedByteArray &p_plaintext, const PackedByteArray &p_aad) {
	PackedByteArray out;
	ERR_FAIL_COND_V(p_key.size() != AEAD_KEY_SIZE, out);
	ERR_FAIL_COND_V(p_nonce.size() != AEAD_NONCE_SIZE, out);

	out.resize(p_plaintext.size() + AEAD_TAG_SIZE);

	mbedtls_gcm_context ctx;
	mbedtls_gcm_init(&ctx);
	bool ok = false;
	do {
		if (mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES,
					(const unsigned char *)p_key.ptr(), AEAD_KEY_SIZE * 8) != 0) {
			break;
		}
		if (mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, (size_t)p_plaintext.size(),
					(const unsigned char *)p_nonce.ptr(), AEAD_NONCE_SIZE,
					(const unsigned char *)p_aad.ptr(), (size_t)p_aad.size(),
					(const unsigned char *)p_plaintext.ptr(),
					(unsigned char *)out.ptrw(),
					AEAD_TAG_SIZE, (unsigned char *)out.ptrw() + p_plaintext.size()) != 0) {
			break;
		}
		ok = true;
	} while (false);
	mbedtls_gcm_free(&ctx);

	if (!ok) {
		ERR_PRINT("AIRemoteCrypto: GCM encrypt failed.");
		return PackedByteArray();
	}
	return out;
}

bool AIRemoteCrypto::aes_gcm_decrypt(const PackedByteArray &p_key, const PackedByteArray &p_nonce,
		const PackedByteArray &p_ciphertext, const PackedByteArray &p_aad, PackedByteArray &r_plaintext) {
	ERR_FAIL_COND_V(p_key.size() != AEAD_KEY_SIZE, false);
	ERR_FAIL_COND_V(p_nonce.size() != AEAD_NONCE_SIZE, false);
	if (p_ciphertext.size() < AEAD_TAG_SIZE) {
		return false;
	}

	const int body = p_ciphertext.size() - AEAD_TAG_SIZE;
	PackedByteArray plain;
	plain.resize(body);

	mbedtls_gcm_context ctx;
	mbedtls_gcm_init(&ctx);
	bool ok = false;
	do {
		if (mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES,
					(const unsigned char *)p_key.ptr(), AEAD_KEY_SIZE * 8) != 0) {
			break;
		}
		// auth_decrypt verifies the tag in constant time and returns non-zero
		// on mismatch without releasing plaintext.
		if (mbedtls_gcm_auth_decrypt(&ctx, (size_t)body,
					(const unsigned char *)p_nonce.ptr(), AEAD_NONCE_SIZE,
					(const unsigned char *)p_aad.ptr(), (size_t)p_aad.size(),
					(const unsigned char *)p_ciphertext.ptr() + body, AEAD_TAG_SIZE,
					(const unsigned char *)p_ciphertext.ptr(),
					(unsigned char *)plain.ptrw()) != 0) {
			break;
		}
		ok = true;
	} while (false);
	mbedtls_gcm_free(&ctx);

	if (!ok) {
		return false;
	}
	r_plaintext = plain;
	return true;
}

// ---------------------------------------------------------------------------
// Comparison + encoding
// ---------------------------------------------------------------------------

bool AIRemoteCrypto::const_time_equals(const PackedByteArray &p_a, const PackedByteArray &p_b) {
	// Length is not secret, so returning early on a mismatch is fine. The
	// content comparison must not short-circuit: an attacker who can time it
	// would otherwise learn the secret one byte at a time.
	if (p_a.size() != p_b.size()) {
		return false;
	}
	if (p_a.is_empty()) {
		return true;
	}
	// volatile stops the compiler turning this back into an early-out memcmp.
	volatile uint8_t diff = 0;
	const uint8_t *a = p_a.ptr();
	const uint8_t *b = p_b.ptr();
	for (int i = 0; i < p_a.size(); i++) {
		diff |= (uint8_t)(a[i] ^ b[i]);
	}
	return diff == 0;
}

String AIRemoteCrypto::b64_encode(const PackedByteArray &p_data) {
	if (p_data.is_empty()) {
		return String();
	}
	return CryptoCore::b64_encode_str(p_data.ptr(), (size_t)p_data.size());
}

PackedByteArray AIRemoteCrypto::b64_decode(const String &p_b64) {
	PackedByteArray out;
	if (p_b64.is_empty()) {
		return out;
	}
	CharString cs = p_b64.utf8();
	// Base64 expands 3 bytes to 4 characters, so this is always sufficient.
	out.resize(cs.length());
	size_t written = 0;
	Error err = CryptoCore::b64_decode(out.ptrw(), (size_t)out.size(), &written,
			(const uint8_t *)cs.get_data(), (size_t)cs.length());
	if (err != OK) {
		return PackedByteArray();
	}
	out.resize((int)written);
	return out;
}

String AIRemoteCrypto::b64url_encode(const PackedByteArray &p_data) {
	String s = b64_encode(p_data);
	s = s.replace("+", "-").replace("/", "_").replace("=", "");
	return s;
}

PackedByteArray AIRemoteCrypto::b64url_decode(const String &p_b64) {
	String s = p_b64.strip_edges().replace("-", "+").replace("_", "/");
	while (s.length() % 4 != 0) {
		s += "=";
	}
	return b64_decode(s);
}

String AIRemoteCrypto::fingerprint(const PackedByteArray &p_public) {
	unsigned char hash[32];
	if (CryptoCore::sha256(p_public.ptr(), (size_t)p_public.size(), hash) != OK) {
		return String();
	}
	String out;
	for (int i = 0; i < 8; i++) {
		out += String::num_int64(hash[i], 16).pad_zeros(2);
	}
	return out;
}
