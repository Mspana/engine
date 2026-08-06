# Phase 2 Plan — Relay transport (built, deliberately not deployed)

## Scope limit

Your instruction was "as much as you can with 2, without exposing anything to the outside
that can be hacked." So Phase 2 delivers the relay **code** and the engine's **outbound
transport**, tested against a relay running on localhost. Nothing is deployed; no public
hostname exists; the engine still opens no inbound port in this mode either.

Deploying the Worker is a deliberate, separate step you take when you choose to — the
instructions are in the relay's README.

## Why the same crypto carries over unchanged

The Phase 1 channel was designed as transport-agnostic: keys come from the device
identities, not from TLS, and the counter check rejects replay/reordering by a hostile
intermediary. That means the relay is **untrusted by construction** — it sees only
ciphertext, and cannot read, forge, replay or reorder anything. Moving from LAN to relay is
therefore a transport swap, not a security redesign.

The one genuinely new property: in LAN mode the engine *accepts* a socket; in relay mode it
*dials out*. Dialling out is strictly safer — nothing can reach the machine unsolicited.

## Components

| Path | Role |
| --- | --- |
| `modules/ai/remote/relay/worker.js` | Cloudflare Worker + Durable Object: pairs two sockets by room id, forwards opaque frames |
| `modules/ai/remote/relay/wrangler.toml` | Worker config (not applied anywhere until you deploy) |
| `modules/ai/remote/relay/local_relay.mjs` | Same protocol as a plain Node script, for local testing without Cloudflare |
| `modules/ai/remote/relay/README.md` | How to test locally, and the deploy steps for later |
| `modules/ai/remote/ai_remote_client.{h,cpp}` | Engine-side outbound WebSocket client (RFC6455 client role: masking, `Sec-WebSocket-Key`) |

## Relay design

A Durable Object instance per **room**. The room id is `HMAC-SHA256(server_public_key)`
truncated — derived from the server's identity, so the engine and a paired client can both
compute it without the relay learning either key. Two roles connect: `host` (the engine) and
`guest` (a client). The DO forwards host→guest and guest→host verbatim and keeps no history.

Guarantees the relay provides: reachability and nothing else. Explicit non-goals: it cannot
authenticate anyone (the E2EE layer does that), and it stores no messages.

Cost with WebSocket Hibernation: effectively free at personal scale (Cloudflare's own
example is ~$10/month versus ~$138 without hibernation, and the free tier covers 100k
requests/day).

## Engine-side transport

`AIRemoteClient` reuses `AIRemoteChannel` and the app-message layer verbatim. Differences
from the server path:
- Client-role WebSocket handshake: generate `Sec-WebSocket-Key`, verify the returned
  `Sec-WebSocket-Accept`, and **mask** every outbound frame (RFC6455 requires it).
- It is the *responder* in the session handshake here, because the phone still initiates.
- Reconnects with exponential backoff, since a relay connection is expected to drop.

## Testing without exposure

1. `node modules/ai/remote/relay/local_relay.mjs` — listens on `127.0.0.1:8788` only.
2. Engine connects outbound to `ws://127.0.0.1:8788/room/<id>?role=host`.
3. A browser client connects as `guest` to the same room.
4. Frames must flow end-to-end while the relay only ever handles ciphertext.

Because the relay binds loopback, this exercises the full outbound path with zero exposure.

## Deferred to a real deployment (documented, not built)

- Push notifications (needs a hosted relay plus a Web Push subscription).
- A hosted client page (the Phase 1 embedded page is what gets served today).
- Room-id rotation and relay-side abuse limits.
