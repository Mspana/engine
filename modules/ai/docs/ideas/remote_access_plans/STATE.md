# Remote Access Build — Working State

**Purpose:** survives context compaction. Read this first when resuming.

## User's instructions (VERBATIM — preserve in full through any compaction)

> i'm going to leave you running autonomously to plan, implement, and test phases 0, 1, and 2 (as much as you can with 2, without exposing anything to the outside that can be hacked).
>
> i wont be able to answer questions you should have. when you do have a question, write it down, then continue down the path you recommend. we'll revisit these question checkpoints at the end.
>
> make a plan for each phase, then implement the plan. save the plans locally for posterity, a temporary folder works just fine.
>
> if you are complete before i return, give me a detailed report. describe what you have done, how i can test it, and what i should know.
>
> you have permission to build, but please build in this repo where we have an existing one, so it doesn't take as much resources and time.
>
> keep these instructions in full if you ever compact.

## Standing constraints (from CLAUDE.md / memory — still apply)

- NO commits, ever, unless the user explicitly asks. All work stays in the working tree.
- Build command (matches user's own config to reuse object cache):
  `python -m SCons platform=windows target=editor module_text_server_fb_enabled=yes debug_symbols=yes -j16`
- For test builds use `extra_suffix=tests` (isolated artifacts, no collision):
  add `tests=yes extra_suffix=tests`; run `bin\godot.windows.editor.x86_64.tests.exe --test --test-case="<filter>"`.
  NOTE: tests=yes changes global defines → different object set; extra_suffix isolates it.
- The user's editor exe may be running → linking bin\godot.windows.editor.x86_64.exe may fail
  with Access Denied. The tests-suffixed exe avoids this. If the main link fails due to lock,
  note it in the report; do not kill their editor.
- Machine: 24 logical cores, use -j16.
- Branch: agent-harness (pre-existing uncommitted harness work in tree — do not disturb it).
- Nothing may be exposed to the outside network. LAN listener must be RFC1918/loopback-gated
  and off by default. Phase 2 relay runs locally only (wrangler dev / Node stand-in), no deploys.
- Questions for the user go to QUESTIONS.md in this folder — decide, document, continue.
- Phases defined in ../remote_access.md (the idea doc). Full research context in that doc.

## Phase status — ALL COMPLETE (2026-08-04), uncommitted

- [x] Phase 0: AIChatSession seam + AIChatStore signals + panel executor wiring
- [x] Phase 1: pairing + E2EE + WebSocket server + embedded web client + editor dialog
- [x] Phase 1 tests: 21 doctest cases (124 assertions) pass; 16-check Python E2E over a
      real socket passes
- [x] Phase 2: relay (Worker + Node + Python) and outbound engine transport; 8-check relay
      E2E passes against a loopback-only relay. NOTHING DEPLOYED.
- [x] Docs: modules/ai/docs/architecture/remote_access.md; QUESTIONS.md has 9 decisions

Build state: `bin\godot.windows.editor.x86_64.exe` and the `.tests.exe` both build clean.
Full suite: 1287/1288 pass; the one failure (`AIProvider.send_request` unnamed argument) is
pre-existing at HEAD and unrelated.

## Follow-up work (started 2026-08-04, after Matthew confirmed LAN pairing works)

Plan: `phase3_plan_qr_and_address.md`. Two changes so a phone pairs by scanning:

- [x] LAN address picker. `get_client_url()` took the *first* private IPv4, which on this
      machine can be NordLynx `10.5.0.2` instead of the real `192.168.0.88`. Replaced with
      `score_lan_candidate()` ranking (192.168 > 172.16 > 10, minus a penalty for
      virtual/VPN adapter names, loopback and link-local rejected) plus an override
      dropdown in the dialog. 4 new doctest cases.
- [x] QR wiring: dialog renders the QR, payload is `<url>/#c=<code>` (fragment, never sent
      to the server), client reads it, strips it from history, and auto-pairs.
- [x] QR encoder (`ai_remote_qr.{h,cpp}`): byte mode, EC level M, versions 1–10, max
      payload 213 bytes. Verified three ways — byte-for-byte against golden vectors from
      Python `qrcode` (all ten versions, exact-capacity boundaries), a 1600-payload
      randomised sweep, and every golden matrix decoded back with OpenCV's scanner.

Tests after this work: 39 cases / 237 assertions pass (`--test-case="*AIRemote*"`).

Known state: the main editor binary could not be linked at the end of this session because
Matthew's editor was running and holds `bin\godot.windows.editor.x86_64.exe`. Everything
compiled; only the final link was blocked. Close the editor and rebuild to pick this up.

## Phase 2 verified against the real Worker (2026-08-06)

Node 24 + wrangler 4.119 are now installed on this PC (they were not on 2026-08-04).
`wrangler dev` runs `worker.js` and the `RelayRoom` Durable Object on 127.0.0.1:8787 with
no Cloudflare login and no public hostname. Still NOTHING DEPLOYED.

- [x] 14-check relay contract test passes identically against `wrangler dev` (8787) and
      `local_relay.mjs` (8788): healthz, 404/426/400 routing, room-id and role validation,
      relay_hello / peer_here / peer_gone, both-way forwarding, 64 KiB frames, and the
      4004 replacement path. The Node stand-in and the real Worker behave the same.
      Script lives in the session scratchpad; move it into `tools/` if it earns a home.
- [x] `relay_test.py`: **8 passed, 0 failed** against the real Worker. Pairing, session
      handshake, encrypted state, and `can_control: False` on a freshly paired device.
      The engine dials out; no local port is opened.

Tooling hardened while chasing three false alarms, all in the copy-the-code step:

- `decode_pair_code()` in `e2e_test.py` (used by both scripts) replaces a fragile
  `urlsafe_b64decode(code + "=" * (-len(code) % 4))`. It strips whitespace, quotes and an
  `ARISTOTLE_PAIRING_CODE=` prefix (every character of that prefix is *also* valid base64,
  which made a whole-line copy fail far away as "Incorrect padding"), and on bad input
  reports index, codepoint and a hex dump.
- `--code-from <logfile>` on both scripts reads the code straight out of captured engine
  stdout, which removes the human copy step and the 120-second race together. It reads the
  file as bytes and retries with NULs stripped, because PowerShell 5.1's `Tee-Object`
  always writes UTF-16LE and has no `-Encoding` parameter.

Gotchas worth keeping:

- Launch the editor with `-e --path <project>`. Starting at the project manager and then
  opening a project **relaunches the process**; the second instance detaches from the pipe,
  so its pairing code never reaches the log while the first, stale code does. That presents
  as `bad_pairing_code` with everything else passing.
- A wrong code does NOT close the pairing window — only success burns it. A window that is
  gone timed out (`PAIRING_WINDOW_MS` = 120 s). `begin_pairing()` overwrites any previous
  secret, so a code is valid for one engine process only.
- Codes read off a phone screen (remote desktop) get lookalike substitutions at fixed
  positions with the length unchanged. Use `--code-from`.

## Key facts discovered (update as work proceeds)

- AIStatusPanel: modules/ai/editor/ai_status_indicator.{h,cpp} (~5700 lines cpp). Owns
  Ref<AIChatStore> chat_store, creates CodexHarnessDriver via _ensure_harness_driver (~:5017),
  all append_item call sites, checkpoints, thread mapping via EditorSettings project metadata
  ("ai_harness_threads", chat_id) -> thread_id.
- AIChatStore: modules/ai/editor/ai_chat_store.{h,cpp} — UI-free already, JSONL + meta.json,
  canonical item builders static.
- CodexHarnessDriver: modules/ai/harness/codex_harness_driver.{h,cpp} — RefCounted, headless-capable,
  signals: run_started, api_round_started, progress_update, assistant_delta, thinking_delta,
  thinking_done, assistant_item_ready, tool_result_ready, turn_tokens_ready, run_complete,
  checkpoint_recommended, todos_updated, scene_diff_ready, approval_requested, policy_mode_changed.
  All emitted main-thread. Two loops exist: legacy AgenticOrchestrator + harness driver.
- AIResponsesTranslator: modules/ai/harness/responses_translator.{h,cpp} — existing threaded
  localhost HTTP+SSE server, port 4123, hardcoded 127.0.0.1. Reference for server patterns.
- modules/websocket WSLPeer / WebSocketPeer::accept_stream available; core TCPServer;
  CryptoCore::RandomGenerator in core/crypto for entropy.
- Crypto decision: vendor tweetnacl (public domain, single .c) in modules/ai for X25519 +
  XSalsa20-Poly1305 secretbox; tweetnacl-js on the web client (no secure-context requirement,
  works on plain http:// LAN pages, unlike WebCrypto SubtleCrypto).
