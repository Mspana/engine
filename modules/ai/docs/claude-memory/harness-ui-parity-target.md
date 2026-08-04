---
name: harness-ui-parity-target
description: Standing directive — align UX and code with modern agentic interfaces; convention over in-house ideas
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 243075ac-9bbf-4b68-8330-e146edc749f3
  modified: 2026-07-28T22:46:24.303Z
---

During the codex harness replacement (July 2026, `agent-harness` branch), the user set the
direction, then strengthened it into a standing principle:

1. Legacy dev features (per-request token pills and similar diagnostics) do NOT need to be
   reproduced if codex doesn't surface them naturally.
2. "Our goal is to bring the user experience and code in-line with modern agentic
   interfaces. That means modernizing and changing our UI if necessary. We defer to
   standard convention here, and line up with them over maintaining our ideas."
3. "If we have legacy ideas, we can remove/not use them" — active removal of
   legacy-only concepts (UI elements, code paths) is sanctioned, not just deprioritized.

**Why:** The product should feel like the agent UIs users already know (Claude Code,
Codex, Cursor) rather than preserving in-house implementations for their own sake.

**How to apply:** Before touching harness code, read
`modules/ai/docs/architecture/codex_harness.md` — the developer guide with the invariants
(Variant::INT trap, blocking pipes, main-thread tools, single-renderer rule, per-chat
thread reset) and debugging workflow. When a design question arises in panel/driver work,
ask "what does a modern agent UI do here?" and follow that — streaming text (plain text preferred over
bubbles for final assistant messages), thinking display, plan/todo panel, tool cards,
approval flows, Shift+Tab policy cycling. Redesign existing panel elements when they
conflict with convention; don't build adapters to resurrect legacy-loop behaviors. See
[[harness_replacement_plan]] Phase 3 parity note in modules/ai/docs/design/.
