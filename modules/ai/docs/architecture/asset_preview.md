# Asset Preview

## Purpose

The `preview_asset` tool lets the AI see image files before placing them in a scene.
Without it, the AI can only guess at image contents based on filenames. With previews,
it can make informed decisions about which sprite, texture, or icon to use.

## How It Works

The AI calls `preview_asset` with an array of `res://` image paths. For each path:

1. Loads the image via `Image::load()`
2. Resizes to fit within `max_size` (default 128px), preserving aspect ratio
3. Encodes as PNG, then base64
4. Returns the thumbnail as a vision image the AI can see directly

Multiple images can be previewed in a single tool call (batch mode).

### Supported Formats

`.png`, `.jpg`, `.jpeg`, `.bmp`, `.tga`, `.webp`, `.svg`

### Parameters

- `paths` (required, array of strings): Image file paths (`res://...`)
- `max_size` (optional, int): Max dimension in pixels (default 128, range 32-256)

### Typical Workflow

1. AI calls `list_files` to see what assets exist
2. AI calls `preview_asset` on image files it wants to inspect
3. AI uses the visual information to choose the right asset for the task

### Image Delivery

Thumbnails are delivered via the shared tool-result image pipeline -- see
[tool_result_images.md](tool_result_images.md). `preview_asset` uses the `_images`
array shape (rather than `screenshot_b64`) so it can return multiple thumbnails in
one call.

### Key Files

- `modules/ai/actions/read_actions.cpp` -- `exec_preview_asset()` implementation
- `modules/ai/agentic_orchestrator.cpp` -- `_execute_tool_call()` extracts `_images`
- `modules/ai/tools_array.inc` -- tool registration

### Limitations

- Images only. Other asset types (3D models, animations, scenes) are not supported
  and will need different preview approaches.
- Previews are generated on-demand, not cached.
