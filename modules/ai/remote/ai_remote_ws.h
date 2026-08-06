/**************************************************************************/
/*  ai_remote_ws.h                                                        */
/**************************************************************************/
/* Minimal server-side RFC6455 WebSocket: handshake key derivation and    */
/* frame encode/decode over plain buffers (no I/O, so it unit-tests).      */
/*                                                                        */
/* Hand-rolled rather than using modules/websocket because WSLPeer owns    */
/* the connection from its first byte, which would force the client page   */
/* and the socket onto different ports. With a self-signed certificate,    */
/* a second port fails silently in browsers (no prompt on WSS cert         */
/* errors), so serving both from one origin matters.                       */
/**************************************************************************/

#ifndef AI_REMOTE_WS_H
#define AI_REMOTE_WS_H

#include "core/string/ustring.h"
#include "core/variant/variant.h"

class AIRemoteWS {
public:
	enum Opcode {
		OP_CONTINUATION = 0x0,
		OP_TEXT = 0x1,
		OP_BINARY = 0x2,
		OP_CLOSE = 0x8,
		OP_PING = 0x9,
		OP_PONG = 0xA,
	};

	enum ParseResult {
		PARSE_INCOMPLETE, // need more bytes
		PARSE_OK,
		PARSE_ERROR, // protocol violation — close the connection
	};

	struct Frame {
		bool fin = false;
		int opcode = 0;
		PackedByteArray payload;
		int consumed = 0; // bytes taken from the input buffer
	};

	// A single message may exceed this only by being fragmented; the assembled
	// total is capped too. Generous enough for a long transcript, small enough
	// that a hostile peer cannot exhaust memory.
	static const int MAX_FRAME_PAYLOAD = 8 * 1024 * 1024;
	static const int MAX_MESSAGE_SIZE = 16 * 1024 * 1024;

	// base64(SHA1(key + RFC6455 GUID)) for the Sec-WebSocket-Accept header.
	static String compute_accept_key(const String &p_client_key);

	// RFC6455 masking is role-dependent: a client MUST mask every frame it
	// sends, a server MUST NOT. Pass a 4-byte key when acting as a client
	// (relay mode), or nullptr when acting as a server.
	static PackedByteArray build_frame(int p_opcode, const PackedByteArray &p_payload, bool p_fin = true,
			const uint8_t *p_mask = nullptr);
	static PackedByteArray build_close(int p_code, const String &p_reason, const uint8_t *p_mask = nullptr);

	// Parses one frame starting at p_offset. p_require_mask mirrors the role:
	// true when reading from a client, false when reading from a server.
	static ParseResult parse_frame(const PackedByteArray &p_buffer, int p_offset, Frame &r_frame,
			bool p_require_mask = true);

	// Reassembles fragmented messages. Feed it parsed frames in order.
	class MessageAssembler {
		PackedByteArray buffer;
		int message_opcode = 0;
		bool in_progress = false;

	public:
		// Returns true when a complete message is ready in r_payload.
		// Sets r_error on protocol violations.
		bool feed(const Frame &p_frame, PackedByteArray &r_payload, int &r_opcode, bool &r_error);
		void reset();
	};
};

#endif // AI_REMOTE_WS_H
