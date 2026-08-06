# Questions for Matthew — decided autonomously, revisit together

Format: question → what I chose → why. Ordered roughly by how much I'd like your opinion.

---

## Q1. How literal should the "headless session" extraction be?

The idea doc says extract everything out of `AIStatusPanel` so the panel becomes "client
#1". Done literally that is a several-thousand-line refactor of code where persistence and
rendering are interleaved, with real regression risk and no way for me to verify the UI
interactively.

**Chose:** `AIChatSession` as a *seam* — a UI-free hub that owns the store, funnels all
history events, and exposes the exact command API a remote client needs, with the panel
registered as the executor. The remote layer never touches UI code, and the session's public
API is what a full extraction would expose anyway, so that refactor stays drop-in later.

**Why:** the editor process always has the panel (it's an `EditorPlugin`), so nothing in
Phases 1–2 needs panel-less operation. Same architectural benefit at ~70 lines of panel diff
instead of ~3,000.

**Revisit if:** you want the agent to run with the editor UI closed, or several independent
remote sessions at once (worktree-style). Both need the real extraction.

---

## Q2. Which crypto primitives, given the browser has to speak them too?

**Chose:** X25519 + HKDF-SHA256 + AES-256-GCM. C++ uses mbedTLS (already compiled in);
the browser uses WebCrypto, which supports all three natively — **zero third-party
JavaScript in the client**.

**Why:** no vendored crypto on either side, no supply-chain surface, small audit surface.
AES-GCM rather than ChaCha20-Poly1305 only because WebCrypto doesn't implement ChaCha20.

**Consequence:** `crypto.subtle` requires a *secure context*, and `http://` on a LAN IP is
not one. So LAN mode serves HTTPS with a self-signed cert — meaning **a one-time certificate
warning on your phone**. `http://localhost` *is* a secure context, so desktop testing has no
warning, and Phase 2's relay path would be served over real HTTPS.

**Revisit if:** the certificate warning bothers you. The alternative is vendoring
tweetnacl-js (adds a JS dependency, drops TLS, keeps plain `http://`).

---

## Q3. Should a paired phone be able to drive the agent?

This is the security decision I'd most like you to confirm.

**Chose:** no — pairing grants **view-only**. Sending messages, cancelling and answering
approvals each need an *Allow control* toggle flipped per-device **in the editor**.

**Why:** pairing is the step most likely to happen under time pressure or over someone's
shoulder. Keeping "can make the agent act" behind a separate switch on the physical machine
means a stolen or coerced pairing still cannot touch your files. It costs you one extra
click, once per device.

**Revisit if:** that's too much friction. A middle option is auto-granting control to the
first device paired while remote mode is running, and requiring the toggle for later ones.

---

## Q4. Should remote-initiated turns force approval prompts?

**Chose:** no special case — remote turns obey whatever policy mode the editor is in, and
the existing gates (protected paths, always-ask remote VCS, read-before-write) apply
unchanged.

**Why:** the policy engine already lives in the driver, not the UI, so it protects remote
turns for free. Forcing ASK mode remotely would mean a phone in your pocket blocking on a
prompt nobody is looking at. Given Q3 already gates who can send anything, layering a second
restriction seemed like friction without much gain.

**Revisit if:** you'd rather remote turns never run in AUTO mode. That's a small change —
clamp the effective policy when the run originates remotely.

---

## Q5. Should chats mirror, or should remote open its own session?

**Chose:** mirror. The remote client sees the chat the editor is showing; switching chats
remotely switches the panel too.

**Why:** it matches how Claude Code Remote Control and Happy behave (both surfaces live at
once), and it needs no concurrency work. Independent concurrent sessions imply
worktree-style isolation, which is a much bigger feature.

**Revisit if:** you want to kick off a job from your phone without disturbing what you're
looking at on the desktop.

---

## Q6. Roll our own WebSocket layer, or use `modules/websocket`?

**Chose:** hand-rolled server-side RFC6455 (`ai_remote_ws.{h,cpp}`, ~250 lines, unit-tested).

**Why:** Godot's `WSLPeer::accept_stream` takes ownership of the connection from its first
byte, so the client page and the socket would have to live on different ports. With a
self-signed certificate that is a trap: browsers **do not prompt** on certificate errors for
WebSockets, so the socket would fail silently until you separately visited the second port
and accepted the cert. One origin avoids that entirely.

---

## Q7. How far to take Phase 2 without exposing anything?

**Chose:** built the relay (Cloudflare Worker + Durable Object, plus a Node script and a
Python one for local testing) and the engine's outbound transport, and tested the whole path
against a **loopback-only** relay. Nothing deployed; no public hostname exists.

**Why:** your instruction. Deploying is a deliberate act you should take knowingly — the
steps are in `modules/ai/remote/relay/README.md`.

---

## Q8. Two testing conveniences I added — are you happy with them?

`ARISTOTLE_REMOTE=loopback|lan|relay` starts remote mode at boot, and
`ARISTOTLE_REMOTE_PAIR=1` opens a normal pairing window and prints the code to stdout.

These are how I tested headlessly. They call exactly the same functions as the UI buttons,
and the pairing code is still random, single-use and two-minute-expiring — reading it
already requires local access to the process's output. But they are *env vars that turn on a
network feature*, so if you'd rather they only exist in debug builds, say so and I'll gate
them behind `DEBUG_ENABLED`.

---

## Q9. Frame-size ceiling versus Cloudflare's real limit

The protocol allows 16 MiB messages. Cloudflare enforces a smaller per-message cap on
Workers WebSockets, so a very long transcript could fail on a real deployment even though it
works on LAN. I strip inline images already, which removes the main bulk.

**Left as-is** because it only bites after you deploy. The fix when you get there is
chunking `history` across frames.

---

## Q10. QR pairing — where does the code travel?

**Chose:** the URL **fragment** (`https://192.168.0.88:8420/#c=<code>`), stripped from
history by the client the instant it reads it.

**Why:** a query string would put the code in the HTTP request line and leave it in the
address bar and history; fragments are never sent to the server. Combined with the existing
single-use and two-minute expiry, a code that somehow reaches history is already spent.

Security is otherwise unchanged from typing the code. The one real shift is that a QR is
camera-readable at a glance — but it is also on screen for about a second instead of the
half-minute it takes to type 43 characters, so exposure time goes *down*.

---

## Q11. LAN address ranking heuristic

**Chose:** rank rather than take-first — `192.168/16` (300) > `172.16/12` (200) > `10/8`
(100), minus 250 if the adapter name looks virtual or VPN-ish, with loopback and link-local
rejected. Plus a dropdown override in the dialog.

**Why:** your machine has seven non-loopback addresses, and NordLynx holds `10.5.0.2`
alongside the real `192.168.0.88`. The old "first private address wins" logic could
advertise the VPN address, which a phone cannot reach. `10/8` is ranked lowest precisely
because that is where most VPNs live.

**Known weakness:** a corporate LAN genuinely on `10.x` with a VPN also on `10.x` cannot be
told apart by address alone. That is what the override dropdown is for. Name-based demotion
is a heuristic and will occasionally be wrong on unusual adapter names.

---

## Pre-existing issue found (not mine, not fixed)

`tests=yes` currently fails one unrelated ClassDB test: `AIProvider.send_request` is bound
with `D_METHOD("send_request", "user_prompt")` but the method takes two arguments
(`user_prompt`, `context_block`), so argument 2 is unnamed. It is identical at HEAD, so it
predates this work. One-line fix — adding `"context_block"` to the `D_METHOD` — but it's in
a file you have uncommitted changes in, so I left it alone.
