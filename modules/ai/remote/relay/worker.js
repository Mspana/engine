/**
 * Aristotle remote relay — Cloudflare Worker + Durable Object.
 *
 * WHAT THIS IS
 *   A dumb pipe. Two peers — the desktop engine ("host") and a phone/browser
 *   client ("guest") — both dial OUT to this relay and are paired by room id.
 *   Every payload is forwarded byte-for-byte to the other side.
 *
 * WHY IT IS SAFE TO RUN THIS UNTRUSTED
 *   Everything the peers exchange is already end-to-end encrypted at the
 *   application layer (X25519 + HKDF-SHA256 + AES-256-GCM, with a strictly
 *   increasing counter per direction). The relay therefore CANNOT read, forge,
 *   replay or reorder messages: it only ever sees ciphertext, and any tampering
 *   or reordering is rejected by the receiving peer's counter/AEAD checks.
 *
 *   The relay's single contribution is REACHABILITY: because both peers connect
 *   outbound, the user's PC never opens an inbound port.
 *
 * PRIVACY RULE — READ BEFORE EDITING
 *   Message contents are NEVER logged, inspected, parsed, buffered or persisted.
 *   Not in production, not behind a debug flag, not "just for troubleshooting".
 *   If you find yourself wanting to console.log() a payload to debug something,
 *   use local_relay.mjs on your own machine instead. Do not add it here.
 *   The relay also never stores anything: if a peer is absent, its messages are
 *   dropped, because buffering would mean holding user data at rest.
 */

// ---------------------------------------------------------------------------
// Wire contract. The C++ engine and the web client MUST match these exactly.
// ---------------------------------------------------------------------------

/**
 * Room ids are 32-64 characters of base64url. Hex is a strict subset of that
 * alphabet, so one regex covers both accepted encodings. Padding ('=') is not
 * allowed. The room id is the ONLY routing key the relay understands.
 */
const ROOM_ID_RE = /^[A-Za-z0-9_-]{32,64}$/;

/** Exactly two roles exist. */
const ROLE_HOST = "host";
const ROLE_GUEST = "guest";

/** Control messages the relay itself emits (always JSON text). */
const MSG_RELAY_HELLO = "relay_hello"; // { t, peer_present: bool } — sent on connect
const MSG_PEER_HERE = "peer_here"; // { t } — sent to the peer already in the room
const MSG_PEER_GONE = "peer_gone"; // { t } — sent when the other side disconnects

/** Application-level keepalive: literal text "ping" is answered with "pong". */
const KEEPALIVE_PING = "ping";
const KEEPALIVE_PONG = "pong";

/** Close codes. */
const CLOSE_TOO_BIG = 1009; // message exceeded MAX_MESSAGE_BYTES
const CLOSE_POLICY = 1008; // abuse guard tripped
const CLOSE_REPLACED = 4004; // a newer socket took this role in the room

/** Hard message ceiling, mirroring AIRemoteWS::MAX_MESSAGE_SIZE in the engine. */
const MAX_MESSAGE_BYTES = 16 * 1024 * 1024;

/**
 * Abuse guard: a token bucket, which is the cheapest way to express "200/sec
 * sustained" while still tolerating a short burst. Two numbers per socket, no
 * timers, no allocation per message.
 */
const RATE_TOKENS_PER_SEC = 200;
const RATE_BURST_TOKENS = 400; // ~2 seconds of headroom for a reconnect flurry

// ---------------------------------------------------------------------------
// Worker entrypoint: validate, then hand the socket to the room's Durable Object.
// ---------------------------------------------------------------------------

export default {
	/**
	 * @param {Request} request
	 * @param {{ RELAY_ROOMS: DurableObjectNamespace }} env
	 */
	async fetch(request, env) {
		const url = new URL(request.url);

		// Deliberately minimal: no version, no room list, no counts. A health
		// probe must not become an information leak.
		if (url.pathname === "/healthz") {
			return new Response("ok", {
				status: 200,
				headers: { "content-type": "text/plain; charset=utf-8" },
			});
		}

		const match = url.pathname.match(/^\/room\/([^/]+)$/);
		if (!match) {
			return new Response("not found", { status: 404 });
		}

		// Validate the routing key and the role BEFORE checking for the upgrade
		// header: a malformed room id is a client bug worth reporting as 400,
		// and we do not want to spin up a Durable Object for garbage input.
		const roomId = match[1];
		if (!ROOM_ID_RE.test(roomId)) {
			return new Response("bad room id", { status: 400 });
		}

		const role = url.searchParams.get("role");
		if (role !== ROLE_HOST && role !== ROLE_GUEST) {
			return new Response("bad role", { status: 400 });
		}

		if ((request.headers.get("Upgrade") || "").toLowerCase() !== "websocket") {
			return new Response("expected websocket upgrade", { status: 426 });
		}

		// One Durable Object per room. idFromName() is a deterministic hash, so
		// both peers reach the same instance without any shared registry.
		const id = env.RELAY_ROOMS.idFromName(roomId);
		return env.RELAY_ROOMS.get(id).fetch(request);
	},
};

// ---------------------------------------------------------------------------
// The room. One instance per room id, holding at most one host and one guest.
// ---------------------------------------------------------------------------

/**
 * Uses the WebSocket Hibernation API (state.acceptWebSocket + the
 * webSocketMessage/webSocketClose/webSocketError handlers) rather than
 * addEventListener. Hibernation lets the runtime evict this object from memory
 * while sockets stay connected, which is what makes an idle room cost ~nothing.
 *
 * Consequence to keep in mind when editing: in-memory fields do NOT survive
 * hibernation. All durable truth lives in the sockets themselves and their
 * tags, which the runtime restores for us.
 */
export class RelayRoom {
	constructor(state, env) {
		this.state = state;
		this.env = env;

		/**
		 * Rate-limit buckets, keyed by socket. Intentionally in-memory only.
		 * Losing this on hibernation is harmless and not exploitable: a room
		 * only hibernates after it has been idle, and an idle socket is by
		 * definition not exceeding a sustained message rate.
		 * @type {Map<WebSocket, { tokens: number, last: number }>}
		 */
		this.buckets = new Map();

		// Answer a literal text "ping" with "pong" WITHOUT waking the object
		// from hibernation. This is the one and only message the relay does not
		// forward; it exists so peers can keep a connection warm for free.
		// local_relay.mjs implements the same exception, so the two stay
		// interchangeable.
		this.state.setWebSocketAutoResponse(
			new WebSocketRequestResponsePair(KEEPALIVE_PING, KEEPALIVE_PONG)
		);
	}

	/**
	 * Called by the Worker with the original upgrade request. Re-validates the
	 * role rather than trusting the caller — defence in depth is free here.
	 *
	 * Note the room id is never read, stored or logged in this class: routing
	 * already happened via idFromName(), so the object has no need to know it.
	 */
	async fetch(request) {
		const url = new URL(request.url);
		const role = url.searchParams.get("role");

		if (role !== ROLE_HOST && role !== ROLE_GUEST) {
			return new Response("bad role", { status: 400 });
		}
		if ((request.headers.get("Upgrade") || "").toLowerCase() !== "websocket") {
			return new Response("expected websocket upgrade", { status: 426 });
		}

		const pair = new WebSocketPair();
		const client = pair[0];
		const server = pair[1];

		// At most one socket per role. A reconnecting phone must never be locked
		// out by its own half-dead socket, so the NEW connection wins and the
		// stale one is closed with 4004.
		for (const stale of this.state.getWebSockets(role)) {
			try {
				stale.close(CLOSE_REPLACED, "replaced");
			} catch {
				// Already gone; nothing to do.
			}
		}

		// The tag is how we find this socket again after hibernation.
		this.state.acceptWebSocket(server, [role]);

		const peerRole = role === ROLE_HOST ? ROLE_GUEST : ROLE_HOST;
		const peers = this.state.getWebSockets(peerRole);

		// Tell the joiner whether anyone is home...
		this.trySend(
			server,
			JSON.stringify({ t: MSG_RELAY_HELLO, peer_present: peers.length > 0 })
		);
		// ...and tell the peer that someone just arrived.
		for (const peer of peers) {
			this.trySend(peer, JSON.stringify({ t: MSG_PEER_HERE }));
		}

		return new Response(null, { status: 101, webSocket: client });
	}

	/**
	 * The whole point of the service: forward, verbatim, to the other role.
	 *
	 * `message` is a string for text frames and an ArrayBuffer for binary ones.
	 * Passing it straight to send() preserves both the type and the exact bytes,
	 * which matters because the payload is an AEAD frame — a single flipped byte
	 * would fail authentication on the far side.
	 *
	 * Contents are never examined beyond their size. Do not add logging here.
	 */
	async webSocketMessage(ws, message) {
		const isText = typeof message === "string";

		// For binary we know the exact size. For text we compare against the
		// character count, which is a lower bound on the UTF-8 byte length; that
		// is adequate because text is only used for the small JSON handshake,
		// and encoding a multi-megabyte string just to measure it would be
		// wasteful on every single message.
		const size = isText ? message.length : message.byteLength;
		if (size > MAX_MESSAGE_BYTES) {
			this.tryClose(ws, CLOSE_TOO_BIG, "message too large");
			return;
		}

		if (!this.consumeToken(ws)) {
			this.tryClose(ws, CLOSE_POLICY, "rate limit");
			return;
		}

		const role = this.roleOf(ws);
		if (!role) {
			return; // Untagged socket; should be impossible.
		}

		const peerRole = role === ROLE_HOST ? ROLE_GUEST : ROLE_HOST;
		const peers = this.state.getWebSockets(peerRole);

		// No peer: drop it on the floor. Never buffer — this relay is stateless
		// by design, and queueing would mean storing user data.
		for (const peer of peers) {
			this.trySend(peer, message);
		}
	}

	async webSocketClose(ws, code, reason, wasClean) {
		this.handleGone(ws);

		// Complete the closing handshake. Codes 1005/1006/1015 are status codes
		// the protocol never puts on the wire, so substitute a normal close.
		const echo = code >= 1000 && code < 5000 && code !== 1005 && code !== 1006 && code !== 1015 ? code : 1000;
		try {
			ws.close(echo, "closing");
		} catch {
			// Already closed.
		}
	}

	async webSocketError(ws, error) {
		// The error object itself is not logged: it can embed peer-controlled
		// data, and this relay logs nothing about traffic.
		this.handleGone(ws);
	}

	// -- internals ----------------------------------------------------------

	/**
	 * A socket went away. Notify the peer, but keep the peer's socket OPEN so it
	 * can sit and wait for the other side to come back.
	 */
	handleGone(ws) {
		this.buckets.delete(ws);

		const role = this.roleOf(ws);
		if (!role) {
			return;
		}

		// If another socket already holds this role, this close is the tail end
		// of a replacement (see the 4004 path in fetch()). The peer is still
		// being served, so it must NOT be told the peer is gone.
		const sameRole = this.state.getWebSockets(role).filter((s) => s !== ws);
		if (sameRole.length > 0) {
			return;
		}

		const peerRole = role === ROLE_HOST ? ROLE_GUEST : ROLE_HOST;
		for (const peer of this.state.getWebSockets(peerRole)) {
			this.trySend(peer, JSON.stringify({ t: MSG_PEER_GONE }));
		}
	}

	/** Recovers a socket's role from its hibernation tag. */
	roleOf(ws) {
		const tags = this.state.getTags(ws);
		if (tags.includes(ROLE_HOST)) return ROLE_HOST;
		if (tags.includes(ROLE_GUEST)) return ROLE_GUEST;
		return null;
	}

	/**
	 * Token bucket: refills at RATE_TOKENS_PER_SEC, caps at RATE_BURST_TOKENS.
	 * Returns false once a sender has been over the sustained rate long enough
	 * to drain the bucket.
	 */
	consumeToken(ws) {
		const now = Date.now();
		let bucket = this.buckets.get(ws);
		if (!bucket) {
			bucket = { tokens: RATE_BURST_TOKENS, last: now };
			this.buckets.set(ws, bucket);
		}

		const elapsed = (now - bucket.last) / 1000;
		if (elapsed > 0) {
			bucket.tokens = Math.min(
				RATE_BURST_TOKENS,
				bucket.tokens + elapsed * RATE_TOKENS_PER_SEC
			);
			bucket.last = now;
		}

		if (bucket.tokens < 1) {
			return false;
		}
		bucket.tokens -= 1;
		return true;
	}

	trySend(ws, data) {
		try {
			ws.send(data);
		} catch {
			// Peer vanished mid-send; its close handler will tidy up.
		}
	}

	tryClose(ws, code, reason) {
		try {
			ws.close(code, reason);
		} catch {
			// Already closed.
		}
	}
}
