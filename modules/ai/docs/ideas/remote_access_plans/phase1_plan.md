# Phase 1 Plan — LAN-direct remote access MVP

Everything here runs on the local network only. No cloud, no inbound port from the
internet, no deploy.

## Components

| File | Role |
| --- | --- |
| `modules/ai/remote/ai_remote_crypto.{h,cpp}` | X25519, HKDF-SHA256, AES-256-GCM, CSPRNG, constant-time compare (mbedTLS) |
| `modules/ai/remote/ai_remote_devices.{h,cpp}` | Paired-device registry, persisted to `user://ai_remote/devices.json` |
| `modules/ai/remote/ai_remote_protocol.{h,cpp}` | Handshake state machine, frame encode/decode, replay counters |
| `modules/ai/remote/ai_remote_ws.{h,cpp}` | RFC6455 server-side handshake + framing (hand-rolled — see below) |
| `modules/ai/remote/ai_remote_server.{h,cpp}` | TCP listener, TLS, HTTP static serving, connection threads, bridge to `AIChatSession` |
| `modules/ai/remote/webclient/` | The client page (`index.html`, `app.js`, `style.css`) served by the engine |

## Why hand-roll the WebSocket layer

Godot's `WSLPeer::accept_stream` performs the HTTP upgrade itself, so it must own the
connection from the first byte. That makes it impossible to serve the client page *and*
the WebSocket on one port.

One port matters a lot here: with a self-signed certificate, a browser prompts for the
page's origin, and accepting it also covers a WSS connection to that same origin. Split
across two ports, the WSS handshake fails **silently** (browsers do not prompt on
certificate errors for WebSockets) until the user separately visits the second port and
accepts. That is an awful trap for a phone.

So the server reads the HTTP request itself, routes it, and upgrades in place. Server-side
RFC6455 is well-bounded: SHA-1 + base64 accept key, then frame parse/build with client
unmasking, continuation frames, ping/pong and close.

## Transport

- Loopback mode (default): plain HTTP/WS on `127.0.0.1`. `http://localhost` is a secure
  context, so WebCrypto works with no certificate at all — this is the dev/test path.
- LAN mode: HTTPS/WSS on `0.0.0.0`, with a self-signed cert generated on first enable into
  `user://ai_remote/{key.pem,cert.pem}`. Every accepted socket's peer address is checked
  against loopback + RFC1918 ranges and dropped otherwise, so even a misconfigured router
  cannot expose it.

## Cryptographic design

Identity: server has a long-term X25519 keypair (`user://ai_remote/server_key`); each
client device generates its own and keeps it in `localStorage`.

**Pairing** (only while a pairing window is open — 120 s, single use, 5 attempts max):

```
C→S  pair_hello   {dev_pk, name}
S→C  pair_ack     {srv_pk, salt}
     both: k = HKDF-SHA256(ikm = X25519(own_priv, peer_pub) || P,
                           salt, info="aristotle-remote-pair-v1")
C→S  pair_confirm {proof = AES-GCM(k, ctr 0, "pair-confirm"||dev_pk||srv_pk)}
S→C  pair_ok      {device_id}
```

Mixing the pairing secret `P` into the IKM is what authenticates the exchange: an attacker
who can intercept or substitute keys still cannot derive `k` without `P`. `P` is destroyed
on success and never stored.

**Session** (Noise-KK-shaped: mutual auth from the static keys, forward secrecy from the
ephemerals):

```
C→S  hello     {dev_pk, eph_pk, nonce_c}
S→C  hello_ack {eph_pk_s, nonce_s}
     k_c2s || k_s2c = HKDF-SHA256(ikm = ee || es || se || ss,
                                  salt = nonce_c||nonce_s,
                                  info="aristotle-remote-session-v1", 64)
C→S  (encrypted) hello_verify   ← first frame that decrypts IS the proof of identity
S→C  (encrypted) ready
```

**Data frames** (binary WebSocket messages):

```
[0]     version = 1
[1]     type    = 1 (data)
[2..9]  counter (uint64 big-endian)
[10..]  AES-256-GCM ciphertext || 16-byte tag
nonce = 4-byte direction prefix || 8-byte counter    AAD = bytes [0..9]
```

Keys are per-direction and per-session, so nonces can never repeat. The receiver requires a
**strictly increasing** counter, which blocks replay and reordering by a hostile relay —
the property Phase 2 depends on.

## Application messages (JSON inside the encrypted frame)

Client → server: `get_state`, `get_history`, `list_chats`, `send {text}`, `cancel`,
`approve {decision}`, `switch_chat {id}`, `new_chat`, `ping`.

Server → client: `state`, `history`, `chats`, `item {ts,item}`, `delta {kind,text}`,
`run_state {running}`, `approval {info}`, `status {text}`, `error {message}`, `pong`.

## Security posture

- Server **off by default**; explicit toggle, and a visible indicator while it runs.
- **Remote devices are view-only by default.** Sending messages, cancelling and approving
  each require a per-device "allow control" grant flipped *in the editor*, on the desktop.
  Pairing a phone alone never grants the ability to drive the agent.
- Approvals still route through the existing policy engine; remote clients can *answer* an
  approval only with control granted, and protected paths / always-ask VCS tiers are
  untouched.
- Failed handshakes are rate-limited per IP (5/min → temporary block); pairing attempts are
  counted against the window.
- Sessions: 30 min idle, 12 h absolute; revoking a device kills its live sessions.
- Kill switch disables the listener and drops all sessions.
- Audit log at `user://ai_remote/audit.jsonl`: pairings, connects, revocations, and every
  remote-initiated command.
- Accepted trade-off: the client page itself is served unauthenticated (it must load before
  any key exchange). It contains no secrets, and the LAN + RFC1918 gate bounds who can
  fetch it.

## Test plan

Doctest suites (`extra_suffix=tests`), no network required:
- X25519 agreement, HKDF vectors, AES-GCM round-trip + tamper detection, constant-time compare.
- Frame encode/decode round-trip; tampered ciphertext rejected; replayed/stale counter rejected.
- Full pairing handshake against a simulated client; wrong pairing secret rejected.
- Full session handshake; unknown device rejected; revoked device rejected.
- WebSocket frame parser: masked client frames, fragmentation, oversize guard, ping/pong.
- Device registry persistence round-trip.
