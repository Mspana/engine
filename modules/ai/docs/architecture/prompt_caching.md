# Prompt Caching

## Why

Every agentic turn resends the full prompt: the system prompt, all tool schemas, and the
entire conversation history (including screenshots). Most of those bytes are identical to
the previous turn. Provider-side prompt caching lets the API skip re-processing the
repeated prefix — cached tokens cost roughly a tenth of the normal input price and prefill
faster.

## How it works here

Caching is prefix-based: the request is only cacheable up to the first byte that changed.
The module's request shape was already cache-friendly — the system prompt is a static
constant, the tool array is deterministic, conversation history is append-only, and
volatile context (the `[GAME SESSION]` block) is injected as a message near the end rather
than into the system prompt. Nothing here should be reordered or interpolated with dynamic
values without keeping that property in mind.

**Anthropic** requires explicit opt-in, so `AnthropicProvider` attaches two
`cache_control: {type: "ephemeral"}` breakpoints per request:

1. On the system prompt block. Anthropic renders `tools -> system -> messages`, so this one
   marker caches the tool schemas and system prompt together. Per-request context (when
   present) is sent as a separate block *after* the breakpoint so it cannot invalidate the
   static prefix.
2. On the last content block of the last message. Because history is append-only, each
   turn's breakpoint becomes the next turn's cache read point, so hits accrue as the
   agentic loop runs.

**OpenAI-compatible providers** (OpenAI, xAI, Moonshot, Gemini, DeepInfra) apply prefix
caching automatically on their side; the stable request shape is all they need. No markers
are sent to them.

## Verifying

Anthropic responses report `cache_read_input_tokens` (served from cache) and
`cache_creation_input_tokens` (written to cache). These are preserved through the
OpenAI-format translation and flow into the raw API logs / watchtower. From the second
model turn of any Anthropic chat onward, `cache_read_input_tokens` should be large and
growing; if it stays zero, something is mutating the prefix between turns.

`prompt_tokens` in translated usage is the *full* prompt size (uncached + cached), matching
OpenAI semantics, so UI token counts stay meaningful with caching on.

## Caveats

- Cache entries live ~5 minutes. Turns within a run always hit; the first turn after a
  long user pause pays one full-price re-write.
- Caches are per-model. Switching provider or model mid-conversation starts cold.
- Editing or pruning older history invalidates everything after the edit. If history
  rewriting is ever added (e.g. dropping old screenshots), do it once at a run boundary,
  not incrementally mid-run.
- Anthropic looks back at most ~20 content blocks from a breakpoint to find the previous
  one. A single turn that adds more than ~20 blocks (massive parallel tool batches) would
  need an intermediate breakpoint; current tool batch sizes stay well under this.
