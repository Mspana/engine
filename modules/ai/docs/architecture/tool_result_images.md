# Tool Result Images

Several tools return images as part of their result: `run_and_screenshot`,
`capture_2d_viewport`, `capture_3d_viewport`, and `preview_asset`. This doc describes
the shared pipeline that gets those images from the action implementation, through the
orchestrator, across each provider's wire format, into the UI pill, and onto disk.

## Convention: Two Shapes

A tool action returns a Dictionary with `status`, `result`, etc. Images are signalled
one of two ways, both recognized by the orchestrator:

- **`result.screenshot_b64`** (string) -- a single PNG, base64-encoded. Used by
  `run_and_screenshot`, `capture_2d_viewport`, `capture_3d_viewport`. The UI pill
  and on-disk persistence both key off this field.
- **`result._images`** (array of strings) -- one or more PNGs. Used by
  `preview_asset` when batching multiple thumbnails.

Both shapes end up in the same place downstream (the message's `_images` array).
Prefer `screenshot_b64` for single-image tools so the UI pill and `<call_id>.png`
persistence light up automatically.

## Pipeline

1. **Action** returns `{status:"success", result:{..., screenshot_b64:"..."}}`
2. **Orchestrator** (`_execute_tool_call` in `agentic_orchestrator.cpp`) detects the
   image field on success, strips it out of the JSON content stored on the message,
   and moves the base64 onto the message as `message["_images"]` (an Array). The
   stripped `result` gets `screenshot: "<see attached image>"` as a placeholder so
   the model sees *something* when it reads its own tool output back.
3. **Provider adapter** (`ai_provider.cpp`) translates `_images` into the wire format
   that each vendor's API expects when serializing the tool-result message.
4. **UI pill** (`ai_status_indicator.cpp`) renders `screenshot_b64` inline as an
   image preview in the chat stream.
5. **Persistence** (same file) writes `screenshot_b64` to disk as `<call_id>.png`
   under the chat's directory and replaces the field in the stored result with the
   filename, keeping the JSONL transcript small.

## Per-Provider Wire Translation

Each vendor has a different encoding for "the tool returned an image." The adapter
must produce the right shape or vision-capable models won't see the image even
though it's on the message.

| Provider | Role | Shape |
|---|---|---|
| OpenAI / xAI / Parasail | `tool` | `content` is an array: `[{type:"text", text:...}, {type:"image_url", image_url:{url:"data:image/png;base64,..."}}]` |
| DeepInfra | `tool` (string) + synthetic `user` follow-up | Tool message keeps text-only string content; the images go in a follow-up user message: `[{type:"text", text:"Image(s) returned by tool call <id>:"}, {type:"image_url", image_url:{...}}]`. Required because DeepInfra's schema rejects `image_url` parts inside `tool` messages with HTTP 422 (`"content","str"` validation error). The `tool_call_id` pairing stays intact on the original tool message. |
| Anthropic | `user` with `tool_result` block | Block's `content` is an array: `[{type:"text"}, {type:"image", source:{type:"base64", media_type:"image/png", data:"..."}}]` |
| Gemini | `user` with `functionResponse` part | Add sibling `inlineData` parts in the same user content: `{inlineData:{mimeType:"image/png", data:"..."}}` |

Each branch checks `supports_vision()` before emitting image parts -- text-only
models get just the text portion. The OpenAI adapter additionally checks
`supports_image_in_tool_content()` to decide between the inline-array shape and
the DeepInfra split-message shape; subclasses override it (`DeepInfraProvider`
returns false, `ParasailProvider` inherits true).

## Adding a New Image-Returning Tool

1. In the action implementation, return `result.screenshot_b64` (single) or
   `result._images` (multiple).
2. Add the tool name to the token-estimation special case in
   `agentic_orchestrator.cpp` so the budget check doesn't under-count the image.
3. No provider changes needed -- the three adapters already handle any
   `message["_images"]`, regardless of which tool produced it.
4. No UI changes needed for `screenshot_b64` -- the existing pill picks it up.

## Known Limitations

- The UI pill shows exactly one image per tool result (the `screenshot_b64` field).
  Tools returning `_images` with multiple entries are delivered to the model but
  only the first is rendered in the pill today.
- Images are stored as PNG only. No JPEG path; encoder is
  `Image::save_png_to_buffer()`.
- Persistence writes one file per tool call (`<call_id>.png`). Multi-image tool
  calls reuse the call id and would collide -- currently only `preview_asset`
  produces multi-image results and it's not persisted as individual pills.

## Key Files

- `modules/ai/agentic_orchestrator.cpp` -- `_execute_tool_call` strip/attach logic
- `modules/ai/ai_provider.cpp` -- per-provider translation in each provider's
  message-building function
- `modules/ai/editor/ai_status_indicator.cpp` -- UI pill rendering, chat-store
  persistence hook
- `modules/ai/editor/ai_chat_store.cpp` -- `save_screenshot()` writes PNG to disk
