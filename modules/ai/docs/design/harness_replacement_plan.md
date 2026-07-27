# Agentic System Replacement — Investigation & Implementation Plan

*July 2026, `agent-harness` branch. Follow-up to [in_engine_vs_external_agents.md](in_engine_vs_external_agents.md): replace the custom model-facing agent loop with an embeddable open-source harness (Codex CLI is the primary candidate), while keeping everything editor-semantic — tools, chat UI, checkpoints, scene diffs — that constitutes the moat.*

---

## 1. Harness options

### 1.1 Recommendation: Codex CLI via `codex app-server`

`codex app-server` is OpenAI's official rich-client integration surface — a long-lived child process speaking bidirectional JSON-RPC 2.0 (newline-delimited) over stdio. It powers OpenAI's own VS Code extension, Codex Desktop, and mobile clients, and is explicitly pitched for embedding in third-party products (clients exist in Go/Python/Swift/Kotlin — non-Node hosts are first-class). This matches our architecture exactly: the editor spawns it with `OS::execute_with_pipe`, streams events into the chat panel, and answers its approval callbacks.

Protocol highlights that map 1:1 onto features we already built by hand:

| Our custom feature | app-server equivalent |
|---|---|
| Turn loop + retries + caching + compaction | owned by the harness (`turn/start`, auto-compaction w/ `contextCompaction` stream item) |
| Instant cancel | `turn/interrupt` (+ our generation guard as backstop) |
| Mid-run message steering | `turn/steer` (append input to in-flight turn) |
| Streaming into chat UI | `item/agentMessage/delta`, `item/reasoning/textDelta`, `item/started`/`item/completed` |
| Todo panel | `turn/plan/updated` / `plan` items |
| Checkpoint anchoring | `turn/completed` (status `completed`/`interrupted`/`failed`) |
| Chat history / branching | `thread/resume`, `thread/fork`, JSONL rollouts |
| Hidden context injection | `thread/inject_items` or per-turn input assembly |
| Confirmation dialogs | `item/commandExecution/requestApproval`, `item/fileChange/requestApproval` |

Other decisive facts: **Apache-2.0** (vendorable, no "Codex" branding for our own product), ships as a **single native Rust binary on Windows**, and there is direct prior art for exactly this pattern — Hermes Agent (spawns app-server, registers its own tools via MCP), OpenClaw (persona injection via `developer_instructions`), Zed's `codex-acp`, promptfoo.

### 1.2 Exposing our 43 tools — DECIDED: `dynamicTools` (spike-proven 7/27)

- **`dynamicTools` (primary)**: client-declared tools on `thread/start` (`dynamicTools: [{type:"function", name, description, inputSchema}]`; requires `initialize` `capabilities.experimentalApi: true`). Executed via server→client request **`item/tool/call`** (`{callId, threadId, turnId, tool, arguments}`) answered with `{success, contentItems}` where content items include **`{type:"inputImage", imageUrl}` accepting base64 data URLs — image support verified end-to-end** (Kimi K2.6 described a test PNG pixel-accurately). Crucially, dynamic tools **serialize flat** to the model, so they work through third-party providers. No relay process, no second channel — everything rides the app-server pipe the driver already owns.
- **MCP server (fallback / secondary backends)**: on stock 0.145.0, MCP tools are wrapped in a proprietary `namespace`-type tool that third-party Responses backends reject (xAI: 422) or silently drop (LiteLLM) — [codex#23186](https://github.com/openai/codex/issues/23186). Proxy-side flattening fails on the return path because the router strict-matches names ([codex#20652](https://github.com/openai/codex/issues/20652), open; both `mcp__server__tool` and bare names rejected as "unsupported call"). MCP remains fine for OpenAI-native models, doubles as the bridge for Claude Agent SDK/Goose, and a vendored-fork fuzzy-match patch in `resolve_tool_info` is the escape hatch. Image caveat when used: no `structuredContent` alongside image blocks (issue #10334).

Universal image fallback either way: tools save PNGs to disk and the model uses Codex's built-in `view_image`.

### 1.3 Persona, models, auth

- **Persona/guidance**: `developer_instructions` (proven pattern — OpenClaw) + a generated AGENTS.md in the project dir. Never `experimental_instructions_file` full replacement — it wipes the harness's own tool-use baseline (issue #4433). Our `system_prompt.inc` content gets refactored into these layers.
- **Models**: OpenAI-first. BYO providers via `[model_providers.*]` but **Responses API only** (Chat Completions removed Feb 2026); Anthropic/Gemini possible through a LiteLLM proxy but second-class (base prompt is GPT-5.x-tuned). Model dropdown maps to `model/list` + per-turn `model` override.
- **Auth — decided: API keys only.** No ChatGPT OAuth (sidesteps the ToS gray zone entirely — OpenAI declined to bless subscription sign-in for commercial third-party hosts, discussion #8338). Keys flow through the existing `.env` mechanism into the child's environment. Future: Aristotle-managed accounts with our own usage limits (we broker keys server-side — the standard API-platform pattern, expressly permitted by OpenAI's API business terms).
- **Windows sandbox**: native but experimental (restricted tokens + synthetic SIDs; `elevated`/`unelevated` modes), with real open bugs. Mostly moot for us: our tools execute in the editor process; Codex's sandbox only governs its shell tool. Default `workspaceWrite` + approval-driven; **sandbox/approval policy exposed to the user as a cycling control (Shift+Tab, the CLI-agent convention), persisted per project.**

### 1.4 Alternatives (kept warm, not chosen)

| | Verdict |
|---|---|
| **Goose** (Block/Linux Foundation, Apache-2.0, Rust) | Best truly-open fallback if OpenAI policy turns hostile. Embeddable as `goosed` REST/WS daemon or directly as Rust crates; MCP-native; genuinely model-agnostic. Weaker at hard coding tasks than Codex/Claude. |
| **Claude Agent SDK** | Best *secondary backend*: spawn the Claude CLI with `--input-format stream-json` and speak NDJSON over stdio from C++ (community-documented protocol). Reuses the same MCP server and approval UI. Not open source, Claude-only, protocol semi-internal. |
| **OpenCode** (MIT, Bun/TS) | Good documented HTTP+SSE server API, but Bun-on-Windows is its least-exercised platform and governance has churned. |
| **Pi** (MIT, Node) | Philosophically closest (LF-JSONL RPC "for non-Node integrations") but one-maintainer bus factor and no built-in MCP. |
| **ACP (Zed's Agent Client Protocol)** | Not a harness — a harness-*agnostic* client protocol with adapters for Codex, Goose, Claude. A design reference for our internal driver interface so backends stay swappable. |

**Top risks:** (1) protocol drift — no semver guarantee on app-server; mitigate by pinning + vendoring the binary and regenerating `generate-json-schema` output per upgrade; (2) ~~ChatGPT-auth ToS ambiguity~~ — eliminated by the API-keys-only decision; (3) model lock-in gravity — mitigated by keeping the driver interface harness-shaped so Goose/Claude SDK can slot in behind it.

### 1.5 Licensing for commercial distribution

Verified against the repository directly (July 2026): the `LICENSE` file is **stock Apache License 2.0 with no additional terms, riders, or usage restrictions**, and a `NOTICE` file exists (OpenAI Codex copyright 2025; MIT-licensed Ratatui attribution).

What this means for shipping Aristotle commercially with a vendored `codex` binary:

- **Permitted, unconditionally**: commercial use, redistribution, modification, private forking, sublicensing derivative work. Apache-2.0 is non-copyleft — bundling the binary imposes **zero open-sourcing obligations on Aristotle itself**.
- **Patent grant included**: contributors grant an explicit patent license (stronger footing than MIT). Standard retaliation clause: the license terminates only if we sue claiming the software infringes our patents.
- **Obligations (light, mechanical)**: ship the Apache-2.0 license text; preserve the NOTICE contents (a third-party-notices screen or file in the distribution); retain copyright headers. If we ever ship a *modified* build, prominently state that it's modified. An unmodified vendored binary needs only license + NOTICE.
- **Statically linked Rust dependencies** carry their own (MIT/BSD/Apache) notices — generate a consolidated THIRD-PARTY-NOTICES at vendoring time (`cargo-about` upstream, or reuse OpenAI's own distribution notices).
- **No trademark grant**: never brand the feature "Codex" or use OpenAI marks in product identity. Factual nominative references (docs, "bring your OpenAI API key") are fine.
- **The license covers the software only.** Model access rides on OpenAI's API business terms — building commercial products on API keys is the platform's expressly intended use (unlike ChatGPT-subscription auth, which we've dropped). Rate-limit and usage-policy compliance is ours to manage, which aligns with the future Aristotle-managed-accounts plan.
- Context: the engine fork itself is Godot (MIT — notices must likewise be preserved), so the full stack is cleanly distributable as a proprietary commercial product with an attribution file.

---

## 2. Current architecture — what stays, what goes

### 2.1 Component inventory

| Component | Files | Fate |
|---|---|---|
| Agentic orchestrator (turn loop, tool-call parsing, retries, cancellation, mid-run injection) | `agentic_orchestrator.cpp/.h` | **Replaced** by a harness driver |
| Provider layer (9 providers, HTTP on worker threads, request assembly, caching) | `ai_provider.cpp/.h` | **Bypassed** (harness owns model calls); possibly retained for legacy fallback toggle |
| Tool executors (stateless `exec_*` per domain) | `actions/*.cpp` | **Kept** — bridged to harness |
| Tool schemas (OpenAI JSON-Schema function defs) | `tools_array.inc` | **Kept** — reused to advertise tools |
| Chat UI (`AIStatusPanel`, 5.5k lines: composer, transcript, checkpoints, model dropdown) | `editor/ai_status_indicator.cpp` | **Kept unchanged** (signal-compatible driver) |
| Chat store (JSONL persistence, canonical items, checkpoints meta) | `editor/ai_chat_store.cpp` | **Kept** |
| Scene diffs (`AISceneDiff` snapshot + Myers diff) | `scene_diff.cpp` | **Kept** |
| Journal / raw API logging (watchtower feeds) | `ai_journal_writer.cpp`, `AI::log_raw_api` | **Kept** — driver logs harness events instead |
| Legacy single-shot JSON-protocol path | `ai.cpp:747-827`, `get_system_prompt` | **Dead weight** — remove during swap |

### 2.2 The seam is clean

Three facts make this swap tractable:

1. **`AI::execute_single_action(Dictionary{action,args}) → Dictionary{status,result|error}`** (`ai.cpp:516`) is a pure dispatch over static executors with zero provider/orchestrator dependency. Anything — including an MCP bridge — can call it, **provided it runs on the main thread** (every executor touches editor singletons).
2. **The UI's only contract with the loop is ~11 signals** (`run_started`, `api_round_started`, `assistant_item_ready`, `tool_result_ready`, `scene_diff_ready`, `run_complete`, `checkpoint_recommended`, …), wired in one place (`ai_status_indicator.cpp:1295-1330`, single call site of `run_agentic_loop` at `:2515`). A replacement driver that re-emits these signals leaves the 5.5k-line panel untouched.
3. **`OS::execute_with_pipe` is fully implemented on Windows** (`platform/windows/os_windows.cpp:1215-1341`): three pipes, `CreateProcessW` with `CREATE_NO_WINDOW`, returns `{stdio, stderr, pid}` with non-blocking `FileAccessWindowsPipe`. Exactly the primitive needed for a long-lived JSON-RPC child process. (Non-blocking reads return partial data — the driver must buffer until a full frame arrives.)

### 2.3 Entanglements the driver must re-home

- **Hidden context injection** lives inside the orchestrator today: `[GAME SESSION]`, `[SCENE CHANGES]` (user edits since last seen), `[SCENE UPDATE]` (post-batch scene diffs), `<user_message>` sandbox wrapping, todo state. The harness driver must inject these itself or the model regresses. See `hidden_model_context.md`.
- **Three async tools break the plain request/execute cycle**: `run_and_screenshot` (timer-driven multi-shot capture), `install_export_templates` (threaded download + extraction), `export_project`/`serve_web_build` (must run outside the message-queue flush). Today they suspend the loop via timer state machines inside the orchestrator. Options: make the bridge hold the tool call open until async completion (harness tolerates long-running tool calls), or reproduce the suspension logic in the driver.
- **No-orphan invariant** (`chat_logging.md`): every persisted `tool_call` must get a paired result — cancels synthesize `status:"cancelled"` results. Any prefix of the store must be a valid conversation. The driver must uphold this when persisting harness events.
- **Instant cancel is a cross-layer protocol** (provider serial gate + orchestrator `_run_gen` generation guard). Equivalent needed for the child process: a stale harness reply must never land in a newer run; cancellation should map to the harness's interrupt, with kill-and-respawn as the fallback.
- **OpenAI wire-format leakage**: history→request translation (`_build_model_messages`) lives in the UI and speaks OpenAI shapes. Drive the harness from the **canonical store items** instead; do not inherit this translation.
- **Prompt caching** is provider-internal today (Anthropic `cache_control` breakpoints, implicit prefix caching for others). Under a harness, context/caching becomes the harness's concern — one entire category of maintenance burden deleted.

### 2.4 Config & secrets

API keys load from process env then a `.env` file (`modules/ai/.env`); model selection persists via `EditorSettings::set_project_metadata("ai", "selected_model", …)`. The driver passes keys through to the child's environment and maps the model dropdown onto harness provider config.

---

## 3. Integration design & implementation phases

### 3.1 Cut line (from code investigation)

Keep `AI::execute_single_action` + `actions/*` + `tools_array.inc` + `AIChatStore` + `AISceneDiff` + checkpoint machinery + the UI signal contract. Replace `AgenticOrchestrator` with a **harness driver** (`RefCounted`) that:

1. Spawns the harness via `OS::execute_with_pipe(blocking=false)`;
2. Speaks its JSON-RPC on a polled worker, marshaling to the main thread via `call_deferred` (mirroring how the provider layer already marshals HTTP completions);
3. Executes tool calls through `execute_single_action` on the main thread;
4. Re-emits the existing orchestrator signals so `AIStatusPanel` needs no changes;
5. Persists canonical items to `AIChatStore` upholding the no-orphan invariant;
6. Injects hidden context (`[GAME SESSION]`, `[SCENE CHANGES]`, `[SCENE UPDATE]`) at turn boundaries.

A `DummyProvider` template exists (`ai_provider.h:332`) if we instead keep the `AIProvider` interface as the boundary — but the orchestrator-replacement cut is cleaner because the harness owns the whole turn loop, not just transport.

### 3.2 Native tool policy — what the harness brings, what we keep, what we block

Codex ships built-in tools: shell command execution, file read/patch (`apply_patch`), `view_image`, web search, and the plan tool. Policy per category:

**Adopt natives (retire our overlapping generic tools).** Script/text editing (`read_script`/`update_script` text mechanics, `list_files`, `copy_file`) is replaced by Codex's battle-tested read/edit/patch machinery. Two editor-side reactions preserve what our versions did better:
- *Parse-error loop*: watch `item/completed` `fileChange` events for `.gd` files; run GDScriptParser+Analyzer editor-side; on errors, feed them back into the turn (`turn/steer` or next-turn context) — same contract as today's `parse_errors` result field.
- *Editor refresh*: trigger `EditorFileSystem` scan on file-change items so the editor reloads scripts immediately.

**Keep as editor tools (the moat).** Everything editor-semantic: scene mutations through the undo stack, `connect_signal`, captures, `run_and_screenshot`, `preview_asset`, `create_sprite_frames`, export tools, project settings, monitored play. These are the tools the harness *can't* have.

**Block natively, redirect to our tools.** Approval callbacks make the editor the enforcement point, not just the prompt:
- `item/fileChange/requestApproval` → **programmatically decline** patches touching `.tscn`/`.tres`/`project.godot`, with a decline reason steering the model to the scene tools ("direct scene-file edits bypass the editor; use update_scene_file / node tools"). Prompt guidance says the same; the callback *enforces* it.
- `item/commandExecution/requestApproval` → shell runs under `sandboxPolicy: workspaceWrite` (writes confined to the project dir, network blocked) with approval routed to an editor dialog. Caveat to accept: read confinement outside the workspace is weak on Windows (sandbox is experimental) — the write confinement + approval dialog is the practical control. Start approval-heavy; relax with an allowlist later.
- Web search: configurable off if unwanted; likely useful (docs lookup) — decide in evaluation.

### 3.3 Process & config topology

```
Godot editor (Aristotle)
 ├─ CodexHarnessDriver (replaces AgenticOrchestrator; same signals)
 │    └─ OS::execute_with_pipe → codex app-server (vendored binary, pinned)
 │         JSON-RPC/JSONL over stdio: initialize → thread/start (dynamicTools from
 │         tools_array.inc schemas) → turn/start → events
 │         ← item/tool/call → call_deferred → AI::execute_single_action (main thread)
 │           → {success, contentItems:[inputText | inputImage data-URL]}
 ├─ generated CODEX_HOME (private dir): config.toml (model_providers/BYO key passthrough,
 │  features/compat flags, approval+sandbox policy), developerInstructions per thread (persona)
 └─ [third-party models only] AIResponsesTranslator (modules/ai/harness/) — native
    in-editor Responses→Chat Completions translation on its own threads;
    localhost listener codex targets via model_providers.aristotle
```

Tools ride the same stdio pipe via `dynamicTools` (spike-proven, §1.2) — no relay
process. An MCP bridge (TCP + stdio shim) is deferred to secondary backends
(Claude Agent SDK, Goose) which consume MCP naturally.

### 3.4 Event → signal mapping (UI unchanged)

| app-server event | Driver action / existing signal |
|---|---|
| `turn/started` | `run_started` / `api_round_started` |
| `item/agentMessage/delta` | streaming text into pending bubble (`progress_update`) |
| `item/reasoning/*Delta` | thinking entry (collapsible) |
| `item/started` (`mcpToolCall`/`dynamicToolCall`/`commandExecution`) | pending tool pill (`assistant_item_ready` w/ tool_call block) |
| `item/completed` (tool) | `tool_result_ready` + canonical tool item persisted (no-orphan) |
| `turn/plan/updated` | `todos_updated` (today's todo panel, driven natively) |
| `contextCompaction` item | info pill (auto-compaction — a backlog item we get for free) |
| `turn/completed` (`completed`) | scene-diff batch flush → `scene_diff_ready`, `run_complete(true)`, `checkpoint_recommended` |
| `turn/completed` (`interrupted`/`failed`) | `run_complete(false)` + synthetic cancelled tool results (no-orphan invariant) |
| approval requests | editor dialogs / auto-policy (§3.2), reply via `serverRequest/resolved` |

Hidden context re-homing: driver prepends `[GAME SESSION]` / `[SCENE CHANGES]` to turn input; `[SCENE UPDATE]` diffs flow via `thread/inject_items` (or next-turn prefix). Mid-run user messages → `turn/steer`. Cancel → `turn/interrupt` + `_run_gen`-style generation guard + kill/respawn fallback. Raw JSON-RPC both directions → `AI::log_raw_api` so the watchtower keeps working unmodified.

### 3.5 Phases

- **Phase 0 — Spike (no engine changes).** Vendored `codex.exe` on Windows driven by a script: handshake, `thread/start`, `turn/start`, event stream; MCP server returning a PNG; `dynamicTools` trial; `turn/interrupt` latency; app-server Windows stability (open bug #26440) on our pinned version. **Go/no-go + tool-transport decision.**

  *Findings so far (July 27, spike lives in `modules/ai/harness_spike/`, pinned `rust-v0.145.0`):*
  - Spawn + stdio JSONL, `initialize` handshake, `model/list`, `thread/start`, `turn/start`, and the event stream all **work on Windows first-try**. Structured turn errors arrive cleanly (`codexErrorInfo`).
  - `codex app-server generate-json-schema` emits the full typed protocol (`schema/v2/*` — the thread/turn surface). Authoritative reference for the C++ driver; regenerate per version bump.
  - **Auth finding**: `OPENAI_API_KEY` in the child env is *not* picked up as auth (401). The driver must call `account/login/start {type:"apiKey", apiKey}` once; credentials persist in `CODEX_HOME/auth.json`. Consequence: **raw frame logging must redact login frames** (spike driver does; watchtower path must too).
  - **Persona finding**: `developerInstructions` and `baseInstructions` are per-`thread/start` params — persona injection needs no config file or AGENTS.md generation at all.
  - Current models (`gpt-5.6-sol`, `gpt-5.6-terra`) report `inputModalities: ["text","image"]` — vision confirmed for screenshot tools.
  - The npm-installed codex (0.104.0) was 40+ versions stale — reinforces the vendored-pin strategy.

  *Findings, second leg (7/27, provider + tools; Moonshot prioritized per user):*
  - **Phase 0 verdict: GO.** All core protocol features validated on Windows: streamed turns, instant interrupt (**0.04s** to `interrupted`), mid-turn steer (redirected an in-flight turn), dynamic tool calls with pixel-accurate image vision. ~30 app-server spawns, zero crashes (bug #26440 never manifested on 0.145.0).
  - **Tool transport decided: `dynamicTools`** — see §1.2. MCP is namespace-wrapped on the wire and unusable with third-party backends on stock codex (#23186/#20652 both still open).
  - **Moonshot Kimi K2.6 works end-to-end** through a local LiteLLM proxy (`/v1/responses` → Chat Completions). Neither api.moonshot.ai nor DeepInfra serve a native Responses endpoint (probed: 404); **xAI has native `/v1/responses`** (early access) as a no-proxy alternative.
  - **Provider-compat kit** required for non-OpenAI backends (all applied in `harness_spike/`):
    `[features] multi_agent = false` (removes namespace-type sub-agent tools — unwanted for Aristotle anyway); `experimental_use_unified_exec_tool = false` (shell as plain function); LiteLLM `drop_params: true` (Moonshot rejects `reasoning_effort`); a transcript-hygiene proxy hook stripping empty assistant messages (Kimi emits them; Moonshot enforces strict tool-call adjacency — the same bug family as our own dangling-tool_calls repair). Open nit: codex's `web_search` tool serialization (`external_web_access`) is rejected by xAI and its off-switch wasn't found; harmless through LiteLLM.
  - **Protocol details for the C++ driver**: `turn/interrupt` requires `{threadId, turnId}`; `turn/steer` requires `{threadId, expectedTurnId, input}`; feature flags queryable via `experimentalFeature/list`; a localhost request-sink provider is invaluable for debugging tool serialization (keep `request_sink.py`).
  - **Native translator parity: CONFIRMED (7/27).** Full spike suite passes through `AIResponsesTranslator` with zero Python in the chain (codex.exe → editor C++ → Moonshot): streamed turn ✓, dynamicTools image round-trip with pixel-accurate vision ✓, interrupt 0.01s ✓, mid-turn steer ✓. Two portable lessons for the Phase 2 driver: (1) **Variant float trap** — JSON-parsed numbers re-stringify as `33.0`, which codex's typed deserializer rejects (it silently dropped `response.completed`, failing turns with "stream disconnected"); every integer field emitted to codex must be cast to `Variant::INT`. (2) **Tool-result images must be hoisted** into a follow-up user message for Chat Completions providers — tool-role messages can't carry images.
  - **Translation-layer decision: RESOLVED (7/27) — native, LiteLLM removed.** Measured sidecar costs killed it: ~570 MB on disk across 16.6k files (vs. localhost latency of ~3 ms median, which was fine), ~257 MB resident, 12–60 s cold start (AV rescans of the Python tree). Replaced by `AIResponsesTranslator` (`modules/ai/harness/responses_translator.{h,cpp}`): localhost HTTP/SSE server on its own threads (no editor-state access), Responses→Chat request translation, the full spike compat kit ported (param dropping, call-id sanitization, blank-assistant stripping, tool-adjacency repair with synthesized no-orphan results, non-function tool filtering), streaming SSE emitter mirroring the golden fixtures in `harness_spike/fixtures/` (captured from LiteLLM output that codex verifiably accepted, terminated by `data: [DONE]`). Spike-gated startup: `ARISTOTLE_TRANSLATOR_PORT` env var (register_types); the Phase 2 driver takes over lifecycle later. Upstream registry seeded with Moonshot Kimi models; keys via env or the module's `.env`.
- **Phase 1 — Tool bridge.** `AIMcpBridge` (or dynamicTools handler) exposing the editor tool surface; schema conversion from `tools_array.inc`; async-tool handling (bridge holds calls open; port the orchestrator's timer state machines for `run_and_screenshot`, `install_export_templates`, `export_project`).
- **Phase 2 — Driver.** `CodexHarnessDriver` behind the existing signal contract; config/AGENTS.md generation; hidden-context injection; cancel/steer; store persistence with no-orphan invariant; checkpoint + scene-diff wiring; raw logging.
- **Phase 3 — Parity & policy.** Model dropdown ↔ `model/list`; parse-error reaction hook; approval dialogs; native-tool policy enforcement (§3.2); user-facing sandbox/approval policy cycling (Shift+Tab, per-project persistence); **project-setting toggle: legacy orchestrator ↔ harness driver** for A/B.
  *Parity target (decided 7/27): a modern, expected agent UI — streaming text, thinking display, plan/todo panel, tool cards, approvals — takes priority over feature-for-feature parity with the legacy loop. Legacy dev diagnostics (e.g. per-request token pills) are dropped where codex has no natural equivalent rather than reconstructed.*
- **Phase 4 — Evaluation gate.** Benchmark suite (moon-face animation rebuild, background-fix task, export flow) old vs new: quality, latency, robustness, cost. Decide default; then delete the legacy loop + provider zoo (retry logic, caching, truncation — entire maintenance categories gone).

### 3.6 What we stop maintaining

Auto-compaction (currently on the backlog — comes free), context-window truncation, transient-retry logic, prompt caching per provider, provider catalog upkeep, tool-call transcript repair, JSON-protocol fallback. Every one is a backlog item or past bug in this module.
