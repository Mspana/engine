# In-Engine Agent vs. External Agents — Strategy

*July 2026. Prompted by reviewing a chat where the AI hand-rolled a frame animation (texture-swapping coroutines, a 24-ColorRect "pixel art" mouth, a runtime-generated shader) instead of using SpriteFrames/AnimationPlayer — which raised the question: does the in-engine agent actually have durable advantages over pointing an external agentic CLI (Claude Code, Codex) at the project folder with run/screenshot support bolted on?*

## The short answer

The defensible value of Aristotle's AI is real, but it is narrower than "the framework." It is the **live-editor tool surface and the non-developer UX** — not the agent harness. The harness (agent loop, context management, compaction, retries, caching, cancellation) is the part where we are rebuilding what external tools already do better, and it is the most replaceable part of what exists today.

## What external harnesses do better

- **Harness maturity.** Our backlog is a list of problems Claude Code/Codex solved years-equivalent ago: auto-compaction, cancellation semantics, resiliency to network failures, context visibility, transcript repair. Every hour spent here is undifferentiated heavy lifting that depreciates as fast as the labs ship.
- **Raw capability.** External agents have unrestricted file writes (including hand-authoring `.tscn` sub-resources like SpriteFrames), shell, git, subagents, and web access. The in-engine agent has exactly its ~43 tools. The animation failure was a tool-surface gap: script-driven animation was the path of least resistance because sub-resource authoring wasn't first-class.

## What in-engine cannot be replicated externally

Ranked by moat strength (2026-07-21 discussion):

1. **The feedback loop.** Engine-side capture of the running game and the 2D/3D editor viewports, immune to window focus/occlusion (OS-level screen grabs — the only option for an external tool — proved unreliable and were abandoned). Extends to monitored play: sampling live property values during gameplay. Agents are only as good as their feedback loop, and this one cannot be built from outside the process.
2. **Editor/engine-specific functionality and co-presence.** The agent works in the same live capacity as the user: same open scene tabs (including unsaved/dirty state), same undo stack, same selection, checkpoints with one-click revert, parse errors from the actual GDScript analyzer at write time, runtime errors from the debugger. This makes the agent feel like an extension of what the user is doing rather than text that spits out a result — and it is structurally unavailable to an external process that only sees saved files.
3. **The tool stack.** The least moat-like: much of it mirrors what external harnesses do natively. See the split below.

## The product argument

The market for "Cursor for games" includes designers and hobbyists who will never install a CLI or manage an API key. For them, chat-in-editor with visual checkpoints *is* the product. Cursor is the proof: an agent loop bolted to an editor they controlled, and owning the whole app is what let them own the experience. Many VS Code extensions offered AI; the fork that owned the app won.

**Rejected: MCP as the product surface.** Exposing Aristotle as an MCP server for users' own Claude Code sessions would make Aristotle a peripheral to someone else's product — they own the user relationship, the workflow, the billing. (MCP as *internal plumbing* — how an embedded harness calls editor tools — is fine and invisible.)

## Direction: open harness, owned surface

Explore running the in-editor chat on an embeddable harness we don't maintain, keeping Aristotle's UX, branding, and editor tool surface:

- **Codex CLI** — Apache-2.0, Rust, supports custom model providers via config (OpenAI-compatible Responses API endpoints, local models via Ollama/LM Studio). A genuine open substrate candidate.
- **Claude Agent SDK** — embeddable but **not open source**: it spawns the Claude CLI as a subprocess and is locked to Anthropic models. Viable as *one backend*, not as *the substrate*.
- **Open alternatives** — Pi, Goose (Block), OpenCode: open source, model-agnostic, designed for embedding/extension. Vercel's AI SDK v7 "HarnessAgent" (June 2026) offers one programmatic interface over Claude Code, Codex, and Pi.

The safest architecture is a narrow Aristotle-owned harness interface (start session, stream events, bridge tool calls, cancel, anchor checkpoints) with adapters behind it — so no single harness becomes a dependency we can't escape.

### The tool-stack split

The current tools divide into two categories with different fates:

- **Generic file CRUD** (`read_script`, `update_script`, `list_files`, `read_scene_file`, `update_scene_file`, `copy_file`, …) — replaceable by harness-native tools, which are better at exact-match editing, diffing, and retry behavior. Likely *gain* quality by swapping (e.g. a harness would hand-author SpriteFrames sub-resources in scene text today).
- **Editor-semantic tools** (`set_property` through the undo stack, `connect_signal`, `open_scene`/dirty-tab handling, `run_and_screenshot`, `capture_2d_viewport`/`capture_3d_viewport`, `preview_asset`, future monitored play and sub-resource authoring) — these are moats #1 and #2 expressed as an API. They survive any harness swap.

**Planned experiment:** run the same real task (e.g. the moon-face talking animation) on the current framework vs. a harness-backed prototype where generic tools are the harness's natives and editor-semantic tools are bridged. Measure what is actually lost. Prediction: file operations lose nothing; scene mutations done as raw text lose undo/checkpoint/dirty-safety — locating the moat boundary empirically.

### What a harness swap does not eliminate

Integration glue remains Aristotle's to build: streaming harness events into the panel UI, mapping permission prompts to editor dialogs, anchoring checkpoints to harness turns, cancellation, watchtower ingestion of harness transcripts. Less work than maintaining a harness; not zero. Also watch platform terms: users bringing ChatGPT/Claude subscription auth inside a third-party product may be restricted; BYO API keys via provider config is the safe path.

## Near-term sequencing

1. **Sub-resource authoring tools** (`create_sprite_frames`, animation track editing) — editor-semantic, survive any architecture, and fix the observed animation-quality failure.
2. **System prompt animation guidance** — after the tools exist, so the optimal path is also the easy path.
3. **Harness prototype spike** — separate track; drive one real task end-to-end through an embedded open harness with bridged editor tools.
