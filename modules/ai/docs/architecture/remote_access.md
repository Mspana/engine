# Remote Access

Lets you view and continue Aristotle chats from a phone or another browser. The agent keeps
running on your machine; the remote client is a thin view onto it.

**Off by default.** Nothing listens, nothing dials out, until you turn it on in the AI
panel's **Remote** button.

## The shape of it

Three layers, deliberately separated so the transport can change without touching security:

1. **`AIChatSession`** (`modules/ai/session/`) — a UI-free view of the current chat. It owns
   the chat store, re-broadcasts every history and streaming event, and exposes the commands
   a client needs (send, cancel, approve, switch chat, new chat). The AI panel registers
   itself as the *executor* that carries those commands out, so remote code never touches
   the UI. `AIChatStore` gained `item_appended` / `items_rewritten` / `chat_changed` signals,
   which means every existing persistence path is observable without changing a single
   `append_item` call site.

2. **`AIRemoteServer`** (`modules/ai/remote/`) — the transport and the encrypted protocol.
   It serves the web client, runs the handshakes, and bridges the session to connected
   devices.

3. **The web client** (`modules/ai/remote/webclient/`) — a dependency-free page compiled
   into the binary and served by the engine. It uses the browser's built-in WebCrypto, so
   there is no third-party JavaScript anywhere in the chain.

## Two transports, one security model

**LAN mode.** The engine listens on your local network. Every accepted socket is checked
against loopback and private address ranges and dropped otherwise — even a misconfigured
router cannot expose it. LAN mode serves over HTTPS with a self-signed certificate the
engine generates once; you accept a certificate warning on the phone the first time.

Picking *which* local address to advertise matters more than it sounds: a developer machine
typically has several, and a VPN adapter (NordVPN, Tailscale, WireGuard) usually holds a
`10.x` address that satisfies the private-range test but is unreachable from a phone. So
candidates are ranked rather than taken first-found — `192.168/16` above `172.16/12` above
`10/8`, with virtual and VPN adapters demoted by name, and loopback and link-local
(`169.254`) rejected outright. The dialog exposes the ranked list in a dropdown so you can
override a wrong guess.

**Relay mode.** The engine makes an *outbound* connection to a small relay and holds it
open. No port is opened locally at all. The relay forwards opaque bytes between the engine
and the client and can read nothing.

Both use the identical encrypted channel, because the channel's security comes from the
device keys rather than from the transport. That is what makes the relay safe to be
untrusted, and it is why moving from LAN to relay is a transport swap rather than a
security redesign.

## Trust

A device is identified by an X25519 public key it generates and keeps. The engine keeps its
own long-term key and an allowlist of paired devices.

**Pairing** happens in a two-minute, single-use window you open from the editor. The code it
shows is mixed into the key derivation, so a party without the code — including anything
sitting in the middle — cannot derive the session key. A successful pairing burns the code
immediately.

The dialog shows the code both as text and as a **QR code**, which encodes the address and
the code together so a phone needs neither typed in. The code travels in the URL's
*fragment* (`#c=…`) rather than its query string: fragments are never sent to the server, and
the client strips it via `history.replaceState` the moment it reads it, so it does not linger
in the address bar or session history. Security is otherwise identical to typing it — the
code is still 32 random bytes, single-use, attempt-limited and expiring — and a QR is
actually on screen for about a second rather than the half-minute it takes to type 43
characters.

**Sessions** derive fresh keys on every connection from both the long-term keys (which
proves who both sides are) and a pair of throwaway keys (so recording today's traffic is
useless if a device key leaks later). Messages are numbered, and the receiver requires the
numbers to strictly increase, which is what stops a hostile relay replaying or reordering
anything.

## What a paired device can do

**Nothing but watch, by default.** Pairing a phone grants read access to the transcript and
live output. Sending messages, cancelling runs and answering approval prompts each require
an *Allow control* grant flipped on the desktop, per device. This is the single most
important property of the design: physical access to your machine remains the gate on
anything that can make the agent act.

Control does not bypass the agent's own safety rails. The existing policy engine still
applies in full — protected file types, the always-ask tier for remote VCS operations, and
read-before-write gates behave exactly as they do locally.

Other limits: sessions expire after 30 minutes idle and 12 hours absolute; revoking a device
kills its live session immediately; failed handshakes are rate-limited per address; and
pairings, connections and every remotely-issued command are appended to
`user://ai_remote/audit.jsonl`.

Screenshots and pasted images are stripped from transcripts sent to a remote client — they
are megabytes of base64 and would stall a phone. The tool card still shows that an image
exists.

## Files on disk

Everything lives under `user://ai_remote/`: `server_key.json` (the engine's identity),
`devices.json` (the allowlist), `audit.jsonl`, and the generated `key.pem` / `cert.pem` for
LAN mode. Deleting `server_key.json` invalidates every pairing.

## Testing it

`modules/ai/remote/tools/e2e_test.py` speaks the real protocol over a real socket and checks
the handshakes, view-only enforcement and replay rejection. `relay_test.py` does the same
through a loopback relay. Both need `pip install cryptography`.

For headless runs, `ARISTOTLE_REMOTE=loopback|lan|relay` starts the server at boot and
`ARISTOTLE_REMOTE_PAIR=1` opens a pairing window and prints the code. These use the same
code paths as the UI; they are conveniences, not bypasses.
