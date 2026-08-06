#!/usr/bin/env node
/**
 * Aristotle remote relay — local test harness.
 *
 * Behaves exactly like worker.js (same URL shape, same room/role rules, same
 * control messages, same close codes) so the engine and the web client can be
 * pointed at either one without changing a line of their code.
 *
 * Deliberate constraints:
 *   - Node >= 18, and ONLY the built-in `node:http` and `node:crypto` modules.
 *     No npm install, no lockfile, nothing to audit. That is why the RFC6455
 *     handshake and framing are hand-rolled below.
 *   - Binds 127.0.0.1 ONLY. This is a loopback test harness; it is never
 *     exposed to a network, and it is not hardened for production use.
 *
 * Like the Worker, it never logs, inspects, buffers or stores message payloads.
 * Connection lifecycle events are printed because they are useful when
 * debugging pairing; contents never are.
 *
 * Usage:
 *   node local_relay.mjs
 *   RELAY_PORT=9000 node local_relay.mjs
 */

import { createServer } from "node:http";
import { createHash } from "node:crypto";

// ---------------------------------------------------------------------------
// Wire contract — must stay identical to worker.js.
// ---------------------------------------------------------------------------

const ROOM_ID_RE = /^[A-Za-z0-9_-]{32,64}$/;

const ROLE_HOST = "host";
const ROLE_GUEST = "guest";

const MSG_RELAY_HELLO = "relay_hello";
const MSG_PEER_HERE = "peer_here";
const MSG_PEER_GONE = "peer_gone";

const KEEPALIVE_PING = "ping";
const KEEPALIVE_PONG = "pong";

const CLOSE_NORMAL = 1000;
const CLOSE_PROTOCOL = 1002;
const CLOSE_POLICY = 1008;
const CLOSE_TOO_BIG = 1009;
const CLOSE_REPLACED = 4004;

const MAX_MESSAGE_BYTES = 16 * 1024 * 1024;

const RATE_TOKENS_PER_SEC = 200;
const RATE_BURST_TOKENS = 400;

// ---------------------------------------------------------------------------
// RFC6455 plumbing.
// ---------------------------------------------------------------------------

/** The magic string from RFC6455 §1.3, appended before hashing the client key. */
const WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

const OP_CONTINUATION = 0x0;
const OP_TEXT = 0x1;
const OP_BINARY = 0x2;
const OP_CLOSE = 0x8;
const OP_PING = 0x9;
const OP_PONG = 0xa;

/** base64(SHA-1(clientKey + GUID)) — the Sec-WebSocket-Accept value. */
function computeAcceptKey(clientKey) {
	return createHash("sha1").update(clientKey + WS_GUID).digest("base64");
}

/**
 * Builds a server->client frame. Server frames are never masked (RFC6455 §5.1).
 * Handles the three length encodings: 7-bit, 16-bit and 64-bit.
 */
function buildFrame(opcode, payload, fin = true) {
	const len = payload.length;
	let header;

	if (len < 126) {
		header = Buffer.allocUnsafe(2);
		header[1] = len;
	} else if (len < 65536) {
		header = Buffer.allocUnsafe(4);
		header[1] = 126;
		header.writeUInt16BE(len, 2);
	} else {
		header = Buffer.allocUnsafe(10);
		header[1] = 127;
		// We never send more than 4 GiB, so the high word is always zero.
		header.writeUInt32BE(0, 2);
		header.writeUInt32BE(len, 6);
	}

	header[0] = (fin ? 0x80 : 0x00) | (opcode & 0x0f);
	return Buffer.concat([header, payload]);
}

function buildCloseFrame(code, reason = "") {
	const reasonBytes = Buffer.from(reason, "utf8");
	const payload = Buffer.allocUnsafe(2 + reasonBytes.length);
	payload.writeUInt16BE(code, 0);
	reasonBytes.copy(payload, 2);
	return buildFrame(OP_CLOSE, payload);
}

/**
 * Parses one client->server frame from the head of `buf`.
 *
 * Returns:
 *   null                         -> incomplete, wait for more bytes
 *   { error: { code, reason } }  -> protocol violation, close the connection
 *   { fin, opcode, payload, consumed }
 */
function parseFrame(buf) {
	if (buf.length < 2) return null;

	const b0 = buf[0];
	const b1 = buf[1];
	const fin = (b0 & 0x80) !== 0;
	const rsv = b0 & 0x70;
	const opcode = b0 & 0x0f;
	const masked = (b1 & 0x80) !== 0;
	let len = b1 & 0x7f;
	let offset = 2;

	// We negotiate no extensions, so the reserved bits must be clear.
	if (rsv !== 0) {
		return { error: { code: CLOSE_PROTOCOL, reason: "reserved bits set" } };
	}
	// Client frames MUST be masked. An unmasked one is a protocol violation.
	if (!masked) {
		return { error: { code: CLOSE_PROTOCOL, reason: "unmasked client frame" } };
	}

	if (len === 126) {
		if (buf.length < 4) return null;
		len = buf.readUInt16BE(2);
		offset = 4;
	} else if (len === 127) {
		if (buf.length < 10) return null;
		const high = buf.readUInt32BE(2);
		const low = buf.readUInt32BE(6);
		// Anything needing the high word is far past our ceiling anyway.
		if (high !== 0) {
			return { error: { code: CLOSE_TOO_BIG, reason: "frame too large" } };
		}
		len = low;
		offset = 10;
	}

	// Reject before allocating, so a bogus length header cannot exhaust memory.
	if (len > MAX_MESSAGE_BYTES) {
		return { error: { code: CLOSE_TOO_BIG, reason: "frame too large" } };
	}

	// Control frames may not be fragmented and may not exceed 125 bytes.
	const isControl = (opcode & 0x08) !== 0;
	if (isControl && (!fin || len > 125)) {
		return { error: { code: CLOSE_PROTOCOL, reason: "bad control frame" } };
	}

	const total = offset + 4 + len;
	if (buf.length < total) return null;

	const maskStart = offset;
	const dataStart = offset + 4;
	const payload = Buffer.allocUnsafe(len);
	for (let i = 0; i < len; i++) {
		payload[i] = buf[dataStart + i] ^ buf[maskStart + (i & 3)];
	}

	return { fin, opcode, payload, consumed: total };
}

// ---------------------------------------------------------------------------
// Rooms. Exactly one host and one guest per room id; nothing is persisted.
// ---------------------------------------------------------------------------

/** @type {Map<string, { host: object|null, guest: object|null }>} */
const rooms = new Map();

const peerRole = (role) => (role === ROLE_HOST ? ROLE_GUEST : ROLE_HOST);

function joinRoom(conn) {
	let room = rooms.get(conn.roomId);
	if (!room) {
		room = { host: null, guest: null };
		rooms.set(conn.roomId, room);
	}

	// The newest socket for a role wins: a reconnecting phone must not be
	// locked out by its own dead socket. Flag the old one first so its cleanup
	// does not report the peer as gone.
	const stale = room[conn.role];
	if (stale) {
		stale.replaced = true;
		closeConn(stale, CLOSE_REPLACED, "replaced");
	}

	room[conn.role] = conn;
	conn.room = room;

	const peer = room[peerRole(conn.role)];
	sendJson(conn, { t: MSG_RELAY_HELLO, peer_present: Boolean(peer) });
	if (peer) sendJson(peer, { t: MSG_PEER_HERE });

	console.log(`[relay] ${conn.role} joined room ${short(conn.roomId)} (peer ${peer ? "present" : "absent"})`);
}

function leaveRoom(conn) {
	const room = conn.room;
	if (!room) return;
	conn.room = null;

	// This socket was superseded; the room slot belongs to a newer connection
	// and the peer is still being served, so say nothing.
	if (conn.replaced) return;

	if (room[conn.role] === conn) room[conn.role] = null;

	// Tell the peer, but leave its socket open so it can wait for a reconnect.
	const peer = room[peerRole(conn.role)];
	if (peer) sendJson(peer, { t: MSG_PEER_GONE });

	if (!room.host && !room.guest) rooms.delete(conn.roomId);

	console.log(`[relay] ${conn.role} left room ${short(conn.roomId)}`);
}

/** Room ids are not secrets, but there is no reason to print them in full. */
const short = (roomId) => `${roomId.slice(0, 8)}...`;

// ---------------------------------------------------------------------------
// Connection handling.
// ---------------------------------------------------------------------------

function makeConn(socket, roomId, role) {
	return {
		socket,
		roomId,
		role,
		room: null,
		replaced: false,
		closing: false,
		cleaned: false,
		alive: true,
		buf: Buffer.alloc(0),
		// Fragment reassembly state.
		fragOpcode: 0,
		fragParts: [],
		fragLen: 0,
		// Token bucket (see worker.js for the rationale).
		tokens: RATE_BURST_TOKENS,
		lastRefill: Date.now(),
	};
}

function send(conn, opcode, payload) {
	if (conn.closing || conn.socket.destroyed || !conn.socket.writable) return;
	conn.socket.write(buildFrame(opcode, payload));
}

function sendJson(conn, obj) {
	send(conn, OP_TEXT, Buffer.from(JSON.stringify(obj), "utf8"));
}

function closeConn(conn, code, reason) {
	if (conn.closing) return;
	conn.closing = true;

	if (!conn.socket.destroyed && conn.socket.writable) {
		conn.socket.write(buildCloseFrame(code, reason));
		conn.socket.end();
	}
	// Do not wait forever for a peer that never completes the handshake.
	setTimeout(() => conn.socket.destroy(), 1000).unref();
}

function cleanup(conn) {
	if (conn.cleaned) return;
	conn.cleaned = true;
	leaveRoom(conn);
}

/** Token bucket: refills at RATE_TOKENS_PER_SEC, capped at RATE_BURST_TOKENS. */
function consumeToken(conn) {
	const now = Date.now();
	const elapsed = (now - conn.lastRefill) / 1000;
	if (elapsed > 0) {
		conn.tokens = Math.min(RATE_BURST_TOKENS, conn.tokens + elapsed * RATE_TOKENS_PER_SEC);
		conn.lastRefill = now;
	}
	if (conn.tokens < 1) return false;
	conn.tokens -= 1;
	return true;
}

/** Consumes as many whole frames as are currently buffered. */
function drain(conn) {
	for (;;) {
		if (conn.closing) return;

		const frame = parseFrame(conn.buf);
		if (frame === null) return; // Need more bytes.

		if (frame.error) {
			closeConn(conn, frame.error.code, frame.error.reason);
			return;
		}

		conn.buf = conn.buf.subarray(frame.consumed);
		handleFrame(conn, frame);
	}
}

function handleFrame(conn, frame) {
	switch (frame.opcode) {
		case OP_PING:
			send(conn, OP_PONG, frame.payload);
			return;

		case OP_PONG:
			conn.alive = true;
			return;

		case OP_CLOSE: {
			// Echo the peer's code back to complete the closing handshake.
			// 1005/1006/1015 are status codes that never appear on the wire, so
			// substitute a normal close (same rule as worker.js).
			const raw = frame.payload.length >= 2 ? frame.payload.readUInt16BE(0) : CLOSE_NORMAL;
			const sendable = raw >= 1000 && raw < 5000 && raw !== 1005 && raw !== 1006 && raw !== 1015;
			closeConn(conn, sendable ? raw : CLOSE_NORMAL, "");
			return;
		}

		case OP_TEXT:
		case OP_BINARY:
			if (conn.fragParts.length > 0) {
				closeConn(conn, CLOSE_PROTOCOL, "interleaved message");
				return;
			}
			if (frame.fin) {
				deliver(conn, frame.opcode, frame.payload);
				return;
			}
			conn.fragOpcode = frame.opcode;
			conn.fragParts = [frame.payload];
			conn.fragLen = frame.payload.length;
			return;

		case OP_CONTINUATION: {
			if (conn.fragParts.length === 0) {
				closeConn(conn, CLOSE_PROTOCOL, "unexpected continuation");
				return;
			}
			conn.fragLen += frame.payload.length;
			if (conn.fragLen > MAX_MESSAGE_BYTES) {
				closeConn(conn, CLOSE_TOO_BIG, "message too large");
				return;
			}
			conn.fragParts.push(frame.payload);
			if (!frame.fin) return;

			const payload = Buffer.concat(conn.fragParts, conn.fragLen);
			const opcode = conn.fragOpcode;
			conn.fragParts = [];
			conn.fragLen = 0;
			deliver(conn, opcode, payload);
			return;
		}

		default:
			closeConn(conn, CLOSE_PROTOCOL, "unknown opcode");
	}
}

/**
 * A complete application message. Forward it verbatim to the peer.
 * The payload is ciphertext; it is never decoded, logged or stored.
 */
function deliver(conn, opcode, payload) {
	if (payload.length > MAX_MESSAGE_BYTES) {
		closeConn(conn, CLOSE_TOO_BIG, "message too large");
		return;
	}
	if (!consumeToken(conn)) {
		closeConn(conn, CLOSE_POLICY, "rate limit");
		return;
	}

	// The single message the relay answers instead of forwarding: a literal
	// text "ping" gets a literal "pong". Mirrors the Worker's hibernation
	// auto-response, which lets a peer keep the connection warm for free.
	if (opcode === OP_TEXT && payload.length === KEEPALIVE_PING.length && payload.toString("latin1") === KEEPALIVE_PING) {
		send(conn, OP_TEXT, Buffer.from(KEEPALIVE_PONG, "utf8"));
		return;
	}

	const peer = conn.room ? conn.room[peerRole(conn.role)] : null;
	// No peer: drop it. Never buffer — buffering would mean storing user data.
	if (!peer) return;

	send(peer, opcode, payload);
}

// ---------------------------------------------------------------------------
// HTTP server: /healthz, plus the WebSocket upgrade on /room/<roomId>.
// ---------------------------------------------------------------------------

const STATUS_TEXT = {
	400: "Bad Request",
	404: "Not Found",
	426: "Upgrade Required",
};

/** Writes a plain HTTP response onto a raw upgrade socket, then hangs up. */
function refuse(socket, status, text) {
	const body = Buffer.from(text, "utf8");
	const header = Buffer.from(
		`HTTP/1.1 ${status} ${STATUS_TEXT[status] || "Error"}\r\n` +
			"connection: close\r\n" +
			"content-type: text/plain; charset=utf-8\r\n" +
			`content-length: ${body.length}\r\n\r\n`,
		"utf8"
	);
	// end() rather than write()+destroy(), which can truncate the body.
	socket.end(Buffer.concat([header, body]));
}

/**
 * Validates the request line. Returns { roomId, role } or { status, message }.
 * Same ordering as the Worker: routing key and role first, upgrade check last.
 */
function route(pathname, searchParams) {
	const match = pathname.match(/^\/room\/([^/]+)$/);
	if (!match) return { status: 404, message: "not found" };

	const roomId = match[1];
	if (!ROOM_ID_RE.test(roomId)) return { status: 400, message: "bad room id" };

	const role = searchParams.get("role");
	if (role !== ROLE_HOST && role !== ROLE_GUEST) return { status: 400, message: "bad role" };

	return { roomId, role };
}

const server = createServer((req, res) => {
	const url = new URL(req.url, "http://127.0.0.1");

	if (url.pathname === "/healthz") {
		res.writeHead(200, { "content-type": "text/plain; charset=utf-8" });
		res.end("ok");
		return;
	}

	// A plain GET to a room means the client forgot to upgrade.
	const routed = route(url.pathname, url.searchParams);
	const status = routed.status ?? 426;
	res.writeHead(status, { "content-type": "text/plain; charset=utf-8" });
	res.end(routed.message ?? "expected websocket upgrade");
});

server.on("upgrade", (req, socket, head) => {
	socket.on("error", () => socket.destroy());

	const url = new URL(req.url, "http://127.0.0.1");
	const routed = route(url.pathname, url.searchParams);
	if (routed.status) {
		refuse(socket, routed.status, routed.message);
		return;
	}

	if ((req.headers.upgrade || "").toLowerCase() !== "websocket") {
		refuse(socket, 426, "expected websocket upgrade");
		return;
	}

	const key = req.headers["sec-websocket-key"];
	if (!key || req.headers["sec-websocket-version"] !== "13") {
		refuse(socket, 400, "bad websocket handshake");
		return;
	}

	socket.setNoDelay(true);
	socket.write(
		"HTTP/1.1 101 Switching Protocols\r\n" +
			"upgrade: websocket\r\n" +
			"connection: Upgrade\r\n" +
			`sec-websocket-accept: ${computeAcceptKey(key)}\r\n\r\n`
	);

	const conn = makeConn(socket, routed.roomId, routed.role);

	socket.on("data", (chunk) => {
		conn.buf = conn.buf.length === 0 ? chunk : Buffer.concat([conn.buf, chunk]);
		drain(conn);
	});
	socket.on("close", () => cleanup(conn));
	socket.on("error", () => {
		conn.closing = true;
		cleanup(conn);
	});

	// Join before draining: `head` can already contain a data frame, and a
	// message that arrives before the room slot is claimed would be dropped.
	joinRoom(conn);

	// Bytes the HTTP parser had already read past the end of the handshake.
	if (head && head.length > 0) {
		conn.buf = head;
		drain(conn);
	}
});

// Protocol-level keepalive, so half-open loopback sockets do not linger as
// zombies holding a room slot.
const KEEPALIVE_MS = 30000;
setInterval(() => {
	for (const room of rooms.values()) {
		for (const conn of [room.host, room.guest]) {
			if (!conn || conn.closing) continue;
			if (!conn.alive) {
				conn.socket.destroy();
				continue;
			}
			conn.alive = false;
			send(conn, OP_PING, Buffer.alloc(0));
		}
	}
}, KEEPALIVE_MS).unref();

// ---------------------------------------------------------------------------
// Listen — loopback only, always.
// ---------------------------------------------------------------------------

const PORT = Number(process.env.RELAY_PORT || 8788);
const HOST = "127.0.0.1"; // NEVER 0.0.0.0. This harness is not for exposure.

if (!Number.isInteger(PORT) || PORT < 1 || PORT > 65535) {
	console.error(`[relay] invalid RELAY_PORT: ${process.env.RELAY_PORT}`);
	process.exit(1);
}

server.listen(PORT, HOST, () => {
	console.log(`[relay] listening on ws://${HOST}:${PORT}`);
	console.log("[relay] LOOPBACK ONLY - bound to 127.0.0.1, not reachable from your LAN or the internet.");
	console.log(`[relay] host:  ws://${HOST}:${PORT}/room/<roomId>?role=host`);
	console.log(`[relay] guest: ws://${HOST}:${PORT}/room/<roomId>?role=guest`);
	console.log("[relay] forwards opaque frames only; payloads are never inspected, logged or stored.");
});
