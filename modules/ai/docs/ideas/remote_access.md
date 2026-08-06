# Remote Access ("Remote Mode")

**Status: Phases 0–2 implemented and tested on 2026-08-04 (uncommitted).**
This document is the original research and rationale. For how the shipped feature actually
works, see [`../architecture/remote_access.md`](../architecture/remote_access.md); for the
build plans and the decisions taken along the way, see
[`remote_access_plans/`](remote_access_plans/).

## The idea

When enabled, the local editor instance becomes the host for its AI chats: a mobile or web
client can view live sessions, continue old chats, and start new ones from anywhere. The
agent keeps executing locally — the remote surface is a thin client, like Claude Code's
Remote Control or Happy Coder.

This is a high-stakes feature: the endpoint indirectly controls an agent with file write
and effective code execution on the host PC. The design below treats security as the
primary constraint, not an add-on.

## Core design decision: no inbound ports, ever

The naive version — open a port, hand out a key — recreates the biggest recurring incident
pattern in this space: ~175k Ollama servers found exposed on the internet (half with
code-executing tool calls), ~17k exposed Open WebUIs actively exploited to run miners,
~12.5k exposed MCP servers (40% unauthenticated). The common failure is a localhost
service plus a "remote" toggle that rebinds it to 0.0.0.0.

The proven alternative (used by Anthropic's Remote Control, WhatsApp Web, VS Code Remote
Tunnels, and Happy Coder): **both the engine and the client make outbound WebSocket
connections to a small relay**, which routes messages between them. The PC never listens
on any interface. All traffic is end-to-end encrypted, so the relay only ever sees
ciphertext and metadata — a fully compromised relay cannot read or forge messages.

The same E2EE protocol also works over a direct LAN WebSocket (phone on the same Wi-Fi,
no internet needed), which makes the transport swappable: LAN-direct first, relay later,
identical crypto.

## Pairing and keys (short term — no accounts)

Not one long-lived copied key. Instead, a pairing ceremony (WhatsApp Web / Syncthing model):

1. User opens "Pair device" in the editor → a QR / short code appears containing the
   relay URL, the desktop's public identity key, and a **single-use pairing secret** that
   expires in ~2 minutes and only exists while that screen is open.
2. The client scans/pastes it, generates its own device keypair, and performs a key
   exchange authenticated by the pairing secret (so the relay or a MITM cannot substitute
   keys). The secret is then destroyed.
3. The desktop keeps an allowlist of paired device public keys — with names, last-seen,
   per-device revoke, and expiry (~90–180 days). Losing a phone means revoking one device,
   not rotating a shared key.
4. Each connection does a fresh ephemeral exchange authenticated by both static keys
   (forward secrecy), producing directional session keys with counter-based nonces
   (replay/reordering protection against a hostile relay).

Crypto costs zero new C++ dependencies: mbedTLS is already in-tree and has X25519,
AES-GCM, ChaCha20-Poly1305, HKDF, and constant-time compare. Browser side: WebCrypto now
has X25519 + AES-GCM as baseline (no ChaCha20 — so AES-GCM on the wire), or the audited
tweetnacl-js, which is what Happy uses.

An account system is a **long-term optional layer** (identity, device roster sync, push
routing) — it complements device keys, it does not replace them. Happy ships with no
accounts at all; Anthropic layers device trust (passkeys, biometric step-up) on top of
accounts. Device keys remain the security core either way.

## Hardening requirements

- Auth before parse: unauthenticated connections get silence (no banner, no health JSON),
  then disconnect. Constant-time comparison for every secret check.
- Pairing codes: 3–5 attempts then the code dies; brute-force backoff.
- Sessions: idle timeout + absolute lifetime; bound to a device key so revocation kills
  live sessions instantly.
- **Remote turns default to ask-mode with approvals mirrored to the client.** The existing
  policy system (protected paths, always-ask remote-VCS tier, read-before-write gates)
  lives in the driver, not the UI, so it keeps applying — remote must never silently
  auto-approve.
- Kill switch: one obvious editor control that drops the relay connection and every
  session; a persistent visible indicator while remote mode is active. Remote mode is
  opt-in per launch, never a silent always-on listener.
- Append-only audit log of pairings, connections, and remotely initiated actions (fits
  the existing `ai_journal` pattern).
- The existing localhost translator (port 4123, no auth) must stay hardcoded to
  127.0.0.1 regardless of remote mode.
- Known residual risk: a web-served client is a trust anchor — whoever serves the page
  could serve malicious JS. PWA/native packaging reduces this later; it is the standard
  trade-off every web-delivered E2EE client (including WhatsApp Web) accepts.
- Note: provider API keys live in a plaintext `.env` on disk — remote file-read is a path
  to them, another reason approval gates stay on for remote turns.

## What the engine already has

- `AIChatStore` is already UI-free JSONL with canonical item builders — a remote client
  can render the same items the panel does.
- `CodexHarnessDriver` runs headless (the `ARISTOTLE_HARNESS_SMOKE` path proves it) and
  emits a complete signal contract (`assistant_delta`, `tool_result_ready`,
  `approval_requested`, `run_complete`, …) a bridge can subscribe to.
- The module already runs a threaded localhost HTTP+SSE server (`AIResponsesTranslator`),
  and the editor hosts several TCP servers (LSP, DAP, debugger, file server) as precedent.
  WebSocket server/client and server-side TLS are in-tree.

**The blocking gap:** all persistence and orchestration live in `AIStatusPanel` — every
`append_item`, checkpointing, codex-thread mapping, and the canonical→wire conversion
(`_build_model_messages`, already flagged as debt in `chat_logging.md`). Extracting a
headless session object (owns store + driver + persistence, emits events; the panel
becomes client #1) is the prerequisite for remote access and worthwhile on its own.
OpenCode is the reference for this shape: once the local UI is a client of an internal
session API, a phone is just client #2.

## Prior art (surveyed 2026-08)

| Project | Takeaway |
| --- | --- |
| [Happy Coder](https://github.com/slopus/happy) (MIT, ~23k★) | The blueprint: QR pairing, TweetNaCl E2EE, zero-knowledge self-hostable relay, no accounts. Crypto/pairing design ports directly; client and relay are Claude-Code-shaped, so fork-don't-depend. |
| [Claude Code Remote Control](https://code.claude.com/docs/en/remote-control) | Same topology, best-documented UX: outbound-only polling, QR handoff, phone approvals, presence-aware notifications, worktree-per-remote-session. |
| [Omnara](https://github.com/omnara-ai/omnara) (repo deprecated) | Clean generic "agent reports in" API as a protocol reference; plaintext-to-relay, weaker model. |
| claudecodeui / claude-code-webui / Vibe Kanban / VibeTunnel | Self-hosted UIs and terminal mirrors; mostly no auth, several archived. claudecodeui is AGPL — do not copy code. |

Sobering pattern: four of the surveyed projects were deprecated or archived within about a
year. Reuse designs, not codebases.

## Recommended build

- **Engine bridge (in-house, C++):** subscribes to the headless session's events,
  encrypts, holds one outbound WebSocket. mbedTLS primitives; no new dependencies.
- **Relay (in-house, tiny):** Cloudflare Workers + Durable Objects, one DO per pairing
  routing ciphertext between two sockets. With the WebSocket hibernation API this is
  ~$0–10/month, no server to patch.
- **Client (in-house, web-first):** Pages-hosted web app usable from phone and desktop
  browsers; PWA install later; native only if ever needed.

## Phasing

1. **Phase 0 — extract the headless session** out of `AIStatusPanel`. Pure refactor, pays
   down documented debt, no security surface.
2. **Phase 1 — LAN-direct MVP:** engine-side WSS server (RFC1918-only accept), QR
   pairing, minimal web client. Exercises the full protocol and crypto with zero cloud.
3. **Phase 2 — relay:** Durable Objects relay + Pages-hosted client; same crypto, new
   transport. Push notifications after.
4. **Phase 3 — accounts**, only if multi-user ever matters.
