# Remote relay

A dumb pipe that lets the phone/browser client reach the desktop engine without
the engine ever opening an inbound port.

Two implementations of the same protocol:

| File | Purpose |
| --- | --- |
| `worker.js` | Cloudflare Worker + Durable Object (`RelayRoom`), using the WebSocket Hibernation API. The real thing, if you ever deploy it. |
| `wrangler.toml` | Worker config. Inert until you deliberately deploy. |
| `local_relay.mjs` | The same protocol as a dependency-free Node script bound to `127.0.0.1`. For testing. |

They are interchangeable: point a peer at either and it cannot tell the
difference.

## Trust model

**The relay is untrusted by design, and nothing about the security of the system
depends on it behaving.**

Everything the two peers exchange is already end-to-end encrypted at the
application layer — X25519 key agreement, HKDF-SHA256 key derivation, and
AES-256-GCM with a strictly increasing counter per direction. The relay sees
opaque ciphertext and a room id. It cannot read messages, cannot forge them,
cannot replay or reorder them (the counter check rejects that), and it stores
nothing: if a peer is absent, its messages are dropped rather than queued,
because buffering would mean holding user data at rest.

What the relay actually provides is **reachability, and nothing else**. Both
peers dial *out* to it, so the user's PC never accepts an unsolicited inbound
connection. That is the entire reason this component exists. Moving from LAN
mode to relay mode is a transport swap, not a security change.

A hostile relay's maximum power is denial of service — it can drop your
messages or refuse to pair you. It cannot learn what you said.

## Protocol

```
ws://<relay>/room/<roomId>?role=host      the desktop engine
ws://<relay>/room/<roomId>?role=guest     the phone / browser client
```

- `roomId` — 32-64 characters of base64url (`A-Z a-z 0-9 - _`; hex is a subset).
  Anything else is rejected with **HTTP 400**. This is the only routing key.
- `role` — exactly `host` or `guest`. Anything else is **HTTP 400**.
- Non-WebSocket requests to `/room/*` get **HTTP 426**; unknown paths get **404**.
- `GET /healthz` returns `200 ok` and deliberately nothing else.

One room holds at most one host and one guest. **The newest connection for a
role wins**: an existing socket in that role is closed with **4004 `replaced`**,
so a reconnecting phone is never locked out by its own dead socket.

Peers must **not** request a WebSocket subprotocol (`Sec-WebSocket-Protocol`).
Neither implementation echoes one, and RFC6455 requires a client to fail the
connection if it asked for a subprotocol the server did not confirm. Likewise no
extensions are negotiated, so `permessage-deflate` is never active and frames
carry raw bytes.

Control messages from the relay, always JSON text:

| Message | When |
| --- | --- |
| `{"t":"relay_hello","peer_present":<bool>}` | Sent to a socket immediately on connect. |
| `{"t":"peer_here"}` | Sent to the socket already in the room when the other role joins. |
| `{"t":"peer_gone"}` | Sent when the other role disconnects. Your socket stays open — wait for it to come back. |

Everything else is forwarded verbatim to the other role: binary in, identical
bytes out; text in, identical text out. The one exception is an
application-level keepalive — a message consisting of exactly the text `ping` is
answered with `pong` and is **not** forwarded. (On Cloudflare this is served by
the hibernation auto-responder, so it keeps a connection warm without waking the
Durable Object or costing anything.)

Limits:

| Condition | Close code |
| --- | --- |
| Message larger than 16 MiB | `1009` |
| Sustained sending above ~200 messages/second | `1008` |
| Role taken over by a newer connection | `4004` |

The rate guard is a token bucket: 200 tokens/second, 400 max, one token per
message. Short bursts pass; a sustained flood drains it and the socket is closed.

## Test locally

No install step. Node 18 or newer, nothing from npm.

```sh
node modules/ai/remote/relay/local_relay.mjs
```

It prints that it is bound to `127.0.0.1:8788` and is **not** reachable from
your LAN or the internet. Override the port with `RELAY_PORT`:

```sh
RELAY_PORT=9000 node modules/ai/remote/relay/local_relay.mjs
```

```powershell
# PowerShell
$env:RELAY_PORT = "9000"; node modules/ai/remote/relay/local_relay.mjs
```

Check it is alive:

```sh
curl http://127.0.0.1:8788/healthz     # -> ok
```

Point the two peers at it, using the same room id for both:

- **Engine (host):** connect outbound to
  `ws://127.0.0.1:8788/room/<roomId>?role=host`
- **Client (guest):** connect to
  `ws://127.0.0.1:8788/room/<roomId>?role=guest`

A quick two-terminal smoke test from a browser console or `node --experimental-websocket`:

```js
const a = new WebSocket("ws://127.0.0.1:8788/room/" + "a".repeat(32) + "?role=host");
const b = new WebSocket("ws://127.0.0.1:8788/room/" + "a".repeat(32) + "?role=guest");
a.onmessage = (e) => console.log("host got", e.data);
b.onmessage = (e) => console.log("guest got", e.data);
// b.send("hello") should surface at a, and vice versa.
```

Expected sequence: the first socket gets
`{"t":"relay_hello","peer_present":false}`; the second gets
`{"t":"relay_hello","peer_present":true}` while the first gets
`{"t":"peer_here"}`. Close one and the other gets `{"t":"peer_gone"}` but stays
connected.

You can also run the real Worker locally with `npx wrangler dev` from this
directory, which serves it on localhost too — but that pulls in wrangler and
talks to Cloudflare to authenticate, which is exactly what `local_relay.mjs`
exists to avoid.

## Deploy later

**Deploying makes a public hostname exist.** Right now nothing about this system
is reachable from the internet, and that is a property worth keeping until you
actively want remote access. Treat the steps below as a deliberate decision, not
a setup step.

Before deploying, edit `wrangler.toml`:

1. `name` — becomes part of `https://<name>.<subdomain>.workers.dev`. Pick
   something unremarkable rather than something that advertises what it fronts.
2. `compatibility_date` — bump if you want newer runtime behaviour. Never set a
   future date; Cloudflare rejects those.
3. Optionally `account_id`, if your login has more than one account.
4. Optionally swap `workers_dev` for a custom domain route, so the relay is not
   sitting on a guessable `*.workers.dev` host.

Then:

```sh
cd modules/ai/remote/relay
npx wrangler login          # one-time, opens a browser
npx wrangler deploy         # creates the public hostname
npx wrangler tail           # live request log (lifecycle only; no payloads)
npx wrangler delete         # removes it again
```

The Durable Object migration in `wrangler.toml` (`new_sqlite_classes`) runs
automatically on first deploy. Afterwards, point both peers at
`wss://<your-host>/room/<roomId>?role=...` — note `wss://`, not `ws://`.

Cost: with the Hibernation API an idle room is effectively free, and the free
tier's 100k requests/day is far beyond personal use. Idle connected sockets do
not keep the Durable Object in memory.

One caveat to verify before relying on large frames: Cloudflare enforces its own
per-message size cap on Workers WebSockets, which is smaller than the 16 MiB
ceiling implemented here. Chunk large payloads at the application layer if you
hit it.

## What this deliberately does NOT do

- **No authentication.** The relay will pair anyone who guesses a room id. That
  is fine: without the pairing secret and the device keys, a third party in the
  room sees only ciphertext it cannot decrypt, and its injected frames fail
  authentication at the peer. Authentication is the E2EE layer's job, not the
  relay's — giving the relay credentials to check would make it a thing worth
  attacking.
- **No storage or buffering.** Messages for an absent peer are dropped, never
  queued. Offline delivery would mean storing user data on a machine we do not
  trust.
- **No message inspection, no content logging, ever.** Not behind a debug flag.
  If you need to see traffic while debugging, use `local_relay.mjs` on your own
  machine. This rule is commented in both implementations; please keep it.
- **No push notifications.** Waking a phone requires a hosted relay plus a Web
  Push subscription, which means the relay would learn *when* you are active.
  Future work, if ever, and a deliberate trade.
- **No room-id rotation, no reconnect/backoff policy, no TURN-style fallback.**
  Room ids are derived by the peers; reconnect logic lives in the engine client.
- **`local_relay.mjs` is not production code.** It is a readable test harness:
  loopback-only, no TLS, no connection caps, minimal hardening.
