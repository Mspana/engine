# Codex Harness — Developer Guide

*For agents and humans working on the harness subsystem. Written 7/28/2026 after the
first full increment cycle. Strategy and history: [harness_replacement_plan](../design/harness_replacement_plan.md)
and [in_engine_vs_external_agents](../design/in_engine_vs_external_agents.md).*

## What this is

The harness replaces the custom agentic loop (`AgenticOrchestrator` + `AIProvider`) with
a vendored **codex app-server** child process. The editor keeps everything editor-semantic
(tools, chat UI, checkpoints, scene diffs); codex owns the model loop (context, retries,
compaction, caching). Toggled per-chat via the model dropdown's "(Codex Harness)" entries
— Kimi K2.6 and Kimi K3, both routed through the native translator (dropdown metadata
"codex-harness" or "codex-harness:<model>"; switching harness models resets the driver
and thread/resume re-applies the new model, so chat continuity survives). The legacy loop
still runs for every other dropdown entry until the harness becomes default.

| Component | File | Role |
|---|---|---|
| Driver | `modules/ai/harness/codex_harness_driver.{h,cpp}` | Spawns/talks to codex over JSONL stdio; re-emits the orchestrator signal contract; executes tools |
| Translator | `modules/ai/harness/responses_translator.{h,cpp}` | In-editor localhost HTTP/SSE server translating codex's Responses API to Chat Completions (Moonshot/Kimi) |
| Panel integration | `modules/ai/editor/ai_status_indicator.cpp` (`use_harness_mode`, `harness_*` members) | Streaming, thinking, tool cards, continuity |
| Spike + fixtures | `modules/ai/harness_spike/` | Python protocol driver, request sink, **golden SSE fixtures (the translator's spec)**, generated app-server JSON schema |

## The directive that governs UI decisions

**Bring UX and code in line with modern agentic interfaces. Standard convention beats
in-house ideas; legacy-only concepts may be actively removed** (owner decision, 7/27-28).
Applied so far: assistant prose is borderless rich text, not bubbles; thinking is inline
italic text (the collapsible widget was deleted); codex's native plan tool drives the todo
panel (`update_todos` is no longer declared); per-request token pills are not ported.

## Invariants — break these and things fail quietly

1. **Every integer field emitted to codex must be `Variant::INT`.** JSON-parsed numbers
   are Variant floats and restringify as `33.0`; codex's typed deserializer silently drops
   the event (a dropped `response.completed` fails whole turns with "stream disconnected").
2. **Child pipes are BLOCKING, deliberately.** Non-blocking Windows pipes truncate writes
   over the 4 KB buffer (the 51-tool `thread/start` froze codex mid-frame). Blocking writes
   always complete; reads live on dedicated threads, and **stderr has its own drain thread**
   so the child can never stall on a full pipe.
3. **Tool execution is main-thread only.** The reader thread dispatches frames via
   `call_deferred`; every `exec_*` touches editor singletons. Never call
   `execute_single_action` from a reader/network thread.
4. **Always answer codex's server requests** (`item/tool/call`, approvals) — including on
   cancel and abort paths. A held-open call left unanswered hangs the turn. The
   run_and_screenshot abort path answers with a cancelled result before interrupting.
5. **The panel matches tool cards by `action_id`.** `tool_result_ready` must carry the
   legacy payload shape (`tool_name`/`action_id`/`type`/`args`/`status`/`result|error`/
   `tokens`) built by `_tool_response_from_result`. Wrong field names = "unknown" cards
   duplicating as fail/success pairs.
6. **One renderer per transcript element.** `_create_assistant_text_block` renders
   assistant prose for BOTH live runs and `_rebuild_message_list` reload. Never add a
   second render path — live/reload drift is exactly the antipattern this replaced.
   Same for thinking (`_create_thinking_block` / `_append_thinking_ui`).
7. **Vertical rhythm belongs to `message_list`'s `separation` constant.** Transcript
   blocks must not add their own top/bottom margins.
8. **Thinking lifecycle**: reasoning deltas stream into the current block; the block is
   finalized (and persisted as a `{type:"thinking", text}` store item) when ANY item
   completes — message, tool call, or reasoning item (`thinking_done`). Back-to-back
   thoughts must be separate blocks.
9. **Each chat owns one codex thread.** `_new_chat` AND `_switch_to_chat` must reset the
   driver (missing the new-chat reset caused two chats to share one thread and bleed
   histories). Mapping persists in project metadata section `ai_harness_threads`;
   `thread/resume` re-applies overrides (tools/instructions/model) and falls back to a
   fresh thread if the rollout is gone.
10. **Never log secrets.** The driver redacts `apiKey` in its frame prints; keep that in
    any future raw logging (watchtower ingestion included).

## Approval policy (Shift+Tab)

Codex always runs with `approvalPolicy: "untrusted"` — **the driver is the policy
engine**. `PolicyMode` (cycled via Shift+Tab in the composer, persisted per project as
`ai/harness_policy_mode`) routes each `item/commandExecution/requestApproval` /
`item/fileChange/requestApproval`:

The policy governs **all agent effects uniformly** — codex-native actions AND editor
tools. Editor tools are classified by `_is_read_only_tool` (explicit allowlist of pure
reads; unknown/future tools are gated — safe by default):

- **Ask** (default): reads run freely; every mutating/effectful action — shell, patch,
  or editor tool — raises an approval prompt that REPLACES the composer (modern-CLI
  style; the transcript stays clean, the tool card is the record) with Allow /
  Allow for session / Deny. Gated editor tools are held BEFORE execution and only run
  on accept; "Allow for session" caches per tool name for the driver's lifetime.
  Requests queue; the composer returns when the queue drains (and on cancel/chat
  switch, which also answers the underlying requests).
- **Auto**: everything runs silently.
- **Plan**: read-only enforcement (`readOnly` sandbox for codex, mutating editor tools
  refused) plus per-turn `[PLAN MODE]` instructions: explore with read tools, present a
  numbered plan in chat, ask for confirmation. The plan is chat text (modern
  convention); its steps become live todos via codex's `update_plan` once the user
  switches modes and says go. Codex's native `collaborationMode {mode: plan}` was
  probed on 0.145.0: `thread/start` accepts the field but app-server ignores it
  (byte-identical base prompt — it's a TUI-side feature); re-test on version bumps.

Denied/refused tool calls still answer codex (success:false + explanation) AND resolve
the panel's pending tool card; `developerInstructions` tells the model a denial is user
intent, never to be retried.

**Agent-initiated mode switching** (Claude Code's Enter/ExitPlanMode analog — codex has
no model-invocable mode switch, so these are driver-native dynamic tools):
`enter_plan_mode` switches into plan mode freely (restriction-increasing, no approval;
remembers the prior mode). `exit_plan_mode` ALWAYS raises the composer plan-approval
prompt ("Approve plan and execute?", showing the tool's plan_summary); on approval the
driver restores the pre-plan mode and the model proceeds in the same turn; on decline
the model is told to keep planning. Mode changes sync to the panel label and persist
via the `policy_mode_changed` signal.

Two rules hold in EVERY mode: protected paths (`.tscn`/`.scn`/`.tres`/`.res`/
`project.godot`) are auto-declined — the editor tools are the only path to scenes — and
pending approvals are answered (`cancel`) on run cancel/shutdown, never abandoned.
File-change approvals reference a `fileChange` item by id only; the driver caches those
items from `item/started` to display files and run the guard. Codex's own shell runs get
pending→resolved tool cards like editor tools, so nothing executes invisibly.

## Translator specifics

- The **golden fixtures** in `harness_spike/fixtures/` are captured SSE streams codex
  verifiably accepted — they are the emitter's spec and regression reference. Change the
  emitter only against them.
- Provider-compat kit (all spike-derived, all required for non-OpenAI backends): drop
  `reasoning_effort`-class params; strip blank assistant messages (Kimi emits them,
  Moonshot rejects broken tool-call adjacency); repair adjacency with synthesized
  no-orphan results; sanitize `:` out of call ids; filter non-`function` tools; hoist
  tool-result images into follow-up user messages (chat providers reject images in the
  tool role); streams end with `data: [DONE]`.
- Upstream registry is a static table (`UPSTREAMS`); models with native Responses
  endpoints (OpenAI, xAI) bypass the translator entirely via codex config.
- **Rounds are re-merged on replay.** Codex fragments one model response into separate
  items (reasoning, function_call ×N, message — text recorded *after* the calls it
  announced). `translate_request` accumulates each round's fragments back into the
  single assistant message the model actually produced (`content` + `tool_calls` +
  `reasoning_content`), flushing at tool results / user messages. Without the merge,
  replayed history filled up with text-only assistant messages mid-work, which taught
  Kimi that stopping on "Let me do X:" with no tool call is normal — the premature
  turn-stop bug of Aug 2026.
- **Reasoning round-trips.** Kimi's `reasoning_content` stream becomes a real reasoning
  output item (stable `rs_aristotle_*` id), which codex records in the rollout and
  replays; on replay it is folded back as `reasoning_content` — current-turn rounds
  only, for context economy. Moonshot's thinking models need this to keep their plan
  across tool rounds. Rollback switch: `ARISTOTLE_TRANSLATOR_LEGACY_REASONING=1`
  restores the old deltas-on-message behavior.
- **Truncation is a failure, not a completion.** `finish_reason=length` (Kimi's
  thinking shares the max_tokens budget) emits `response.failed` with code
  `output_limit` so the turn errors visibly instead of silently stopping.
- **Debugging:** `ARISTOTLE_TRANSLATOR_TRACE=1` prints each request's input item types
  and the outgoing message shapes (role, tool count, has-reasoning). Unit tests live in
  `modules/ai/tests/test_responses_translator.h` (`tests=yes` build).

## codex child specifics

- Binary is **vendored and pinned** (`harness_spike/bin/`, rust-v0.145.0). Upgrades:
  regenerate the schema (`codex app-server generate-json-schema`), re-run the spike
  suite, review `[features]` flags. The npm codex was 40+ versions stale — never rely on it.
- `CODEX_HOME` carries config.toml with the compat flags: `[features] multi_agent = false`
  (namespace-type tool, breaks third-party backends), `experimental_use_unified_exec_tool
  = false` (same reason). MCP tools are namespace-wrapped upstream (open issues #23186 /
  #20652) — that's why tools go through **dynamicTools** (flat serialization) instead.
- Protocol details that cost debugging time: `turn/interrupt` needs `{threadId, turnId}`;
  `turn/steer` needs `expectedTurnId`; dynamic tool execution arrives as method
  `item/tool/call` answered with `{success, contentItems:[inputText|inputImage(data URL)]}`;
  auth is `account/login/start {type:"apiKey"}` once (env vars alone don't authenticate).

## Debugging workflow

- `ARISTOTLE_HARNESS_SMOKE="<prompt>"` + headless editor runs one turn at startup with
  full event prints — validates driver changes without the UI.
- `harness_spike/spike_driver.py` (handshake/turn/interrupt/steer/dynamic/mcp tests) talks
  to codex directly; `request_sink.py` captures exactly what codex sends a provider.
- `ARISTOTLE_TRANSLATOR_PORT=4123` starts the translator standalone; curl the fixtures
  through it to diff SSE output against the golden files.
- Build = the F5 task config exactly (`python -m SCons platform=windows target=editor
  module_text_server_fb_enabled=yes debug_symbols=yes`); incremental ~20s. The link fails
  with "Access is denied" while the editor is open — that's the only reason.

## Style notes

- Godot conventions: tabs, `p_` params, `_private` methods, `snake_case`. Comments state
  constraints and non-obvious *whys* (see the pipe-blocking header note), not narration.
- Spike-era env gates (`ARISTOTLE_*`) are temporary scaffolding; the driver is meant to
  own translator/config lifecycles as the harness becomes default. Remove gates as they
  become dead.
- The owner tests interactively before anything is committed; keep increments small and
  independently verifiable.

## State / what's next

**Functionally complete** (7/30). Working end-to-end in the panel: streaming markdown,
inline thinking (persisted), codex-native todos, tool cards, vision (incl.
run_and_screenshot multi-shot), instant cancel, mid-turn steer, per-chat session
continuity, hidden-context injection, the full approval policy (Shift+Tab), and the
export tools (install_export_templates via the shared AITemplateInstaller held-open
flow; export_project/serve_web_build via deferred process_frame dispatch — NEVER a
SceneTreeTimer: EditorProgress pumps Main::iteration and nested process_timers corrupts
the timer list; crash observed and fixed 7/30 in both loops). Remaining: backlog polish
(streaming smoothing, transcript UI items), then flipping the default and removing the
legacy loop.
