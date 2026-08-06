/**************************************************************************/
/*  ai_remote_ws.cpp                                                      */
/**************************************************************************/

#include "ai_remote_ws.h"

#include "core/crypto/crypto_core.h"

#include <string.h>

static const char *WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

String AIRemoteWS::compute_accept_key(const String &p_client_key) {
	String combined = p_client_key.strip_edges() + String(WS_GUID);
	CharString cs = combined.utf8();
	unsigned char hash[20];
	if (CryptoCore::sha1((const uint8_t *)cs.get_data(), (size_t)cs.length(), hash) != OK) {
		return String();
	}
	return CryptoCore::b64_encode_str(hash, 20);
}

PackedByteArray AIRemoteWS::build_frame(int p_opcode, const PackedByteArray &p_payload, bool p_fin,
		const uint8_t *p_mask) {
	PackedByteArray out;
	const int len = p_payload.size();
	const uint8_t mask_bit = p_mask ? 0x80 : 0x00;

	uint8_t header[10];
	int header_len = 0;
	header[0] = (p_fin ? 0x80 : 0x00) | (uint8_t)(p_opcode & 0x0F);

	if (len < 126) {
		header[1] = mask_bit | (uint8_t)len;
		header_len = 2;
	} else if (len <= 0xFFFF) {
		header[1] = mask_bit | 126;
		header[2] = (uint8_t)((len >> 8) & 0xFF);
		header[3] = (uint8_t)(len & 0xFF);
		header_len = 4;
	} else {
		header[1] = mask_bit | 127;
		uint64_t l = (uint64_t)len;
		for (int i = 0; i < 8; i++) {
			header[2 + i] = (uint8_t)((l >> (56 - 8 * i)) & 0xFF);
		}
		header_len = 10;
	}

	const int mask_len = p_mask ? 4 : 0;
	out.resize(header_len + mask_len + len);
	uint8_t *w = out.ptrw();
	memcpy(w, header, header_len);
	if (p_mask) {
		memcpy(w + header_len, p_mask, 4);
	}
	if (len > 0) {
		const uint8_t *src = p_payload.ptr();
		uint8_t *dst = w + header_len + mask_len;
		if (p_mask) {
			for (int i = 0; i < len; i++) {
				dst[i] = src[i] ^ p_mask[i & 3];
			}
		} else {
			memcpy(dst, src, len);
		}
	}
	return out;
}

PackedByteArray AIRemoteWS::build_close(int p_code, const String &p_reason, const uint8_t *p_mask) {
	CharString reason = p_reason.utf8();
	PackedByteArray payload;
	payload.resize(2 + reason.length());
	payload.write[0] = (uint8_t)((p_code >> 8) & 0xFF);
	payload.write[1] = (uint8_t)(p_code & 0xFF);
	if (reason.length() > 0) {
		memcpy(payload.ptrw() + 2, reason.get_data(), reason.length());
	}
	return build_frame(OP_CLOSE, payload, true, p_mask);
}

AIRemoteWS::ParseResult AIRemoteWS::parse_frame(const PackedByteArray &p_buffer, int p_offset, Frame &r_frame,
		bool p_require_mask) {
	const int avail = p_buffer.size() - p_offset;
	if (avail < 2) {
		return PARSE_INCOMPLETE;
	}
	const uint8_t *p = p_buffer.ptr() + p_offset;

	const bool fin = (p[0] & 0x80) != 0;
	const bool rsv = (p[0] & 0x70) != 0;
	const int opcode = p[0] & 0x0F;
	const bool masked = (p[1] & 0x80) != 0;
	uint64_t len = p[1] & 0x7F;

	// No extensions were negotiated, so any reserved bit is a violation.
	if (rsv) {
		return PARSE_ERROR;
	}
	// Masking is mandatory in one direction and forbidden in the other.
	if (masked != p_require_mask) {
		return PARSE_ERROR;
	}
	// Control frames cannot be fragmented and are limited to 125 bytes.
	const bool is_control = (opcode & 0x08) != 0;
	if (is_control && (!fin || len > 125)) {
		return PARSE_ERROR;
	}
	if (opcode != OP_CONTINUATION && opcode != OP_TEXT && opcode != OP_BINARY &&
			opcode != OP_CLOSE && opcode != OP_PING && opcode != OP_PONG) {
		return PARSE_ERROR;
	}

	int cursor = 2;
	if (len == 126) {
		if (avail < cursor + 2) {
			return PARSE_INCOMPLETE;
		}
		len = ((uint64_t)p[cursor] << 8) | (uint64_t)p[cursor + 1];
		cursor += 2;
		if (len < 126) {
			return PARSE_ERROR; // non-minimal length encoding
		}
	} else if (len == 127) {
		if (avail < cursor + 8) {
			return PARSE_INCOMPLETE;
		}
		len = 0;
		for (int i = 0; i < 8; i++) {
			len = (len << 8) | (uint64_t)p[cursor + i];
		}
		cursor += 8;
		if (len <= 0xFFFF) {
			return PARSE_ERROR; // non-minimal length encoding
		}
	}

	if (len > (uint64_t)MAX_FRAME_PAYLOAD) {
		return PARSE_ERROR;
	}

	uint8_t mask[4] = { 0, 0, 0, 0 };
	if (masked) {
		if (avail < cursor + 4) {
			return PARSE_INCOMPLETE;
		}
		memcpy(mask, p + cursor, 4);
		cursor += 4;
	}

	if ((uint64_t)(avail - cursor) < len) {
		return PARSE_INCOMPLETE;
	}

	r_frame.fin = fin;
	r_frame.opcode = opcode;
	r_frame.payload.resize((int)len);
	uint8_t *dst = r_frame.payload.ptrw();
	const uint8_t *src = p + cursor;
	if (masked) {
		for (uint64_t i = 0; i < len; i++) {
			dst[i] = src[i] ^ mask[i & 3];
		}
	} else if (len > 0) {
		memcpy(dst, src, (size_t)len);
	}
	r_frame.consumed = cursor + (int)len;
	return PARSE_OK;
}

// ---------------------------------------------------------------------------
// MessageAssembler
// ---------------------------------------------------------------------------

void AIRemoteWS::MessageAssembler::reset() {
	buffer = PackedByteArray();
	message_opcode = 0;
	in_progress = false;
}

bool AIRemoteWS::MessageAssembler::feed(const Frame &p_frame, PackedByteArray &r_payload,
		int &r_opcode, bool &r_error) {
	r_error = false;

	// Control frames are never fragmented and pass straight through, even in
	// the middle of a fragmented data message.
	if ((p_frame.opcode & 0x08) != 0) {
		r_payload = p_frame.payload;
		r_opcode = p_frame.opcode;
		return true;
	}

	if (p_frame.opcode == OP_CONTINUATION) {
		if (!in_progress) {
			r_error = true; // continuation without a start frame
			return false;
		}
	} else {
		if (in_progress) {
			r_error = true; // new data frame while a message is unfinished
			return false;
		}
		in_progress = true;
		message_opcode = p_frame.opcode;
		buffer = PackedByteArray();
	}

	if (buffer.size() + p_frame.payload.size() > MAX_MESSAGE_SIZE) {
		r_error = true;
		reset();
		return false;
	}
	buffer.append_array(p_frame.payload);

	if (!p_frame.fin) {
		return false;
	}

	r_payload = buffer;
	r_opcode = message_opcode;
	reset();
	return true;
}
