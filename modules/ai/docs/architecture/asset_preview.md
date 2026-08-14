# Asset Preview

## Purpose

The `preview_asset` tool lets the AI see image files before placing them in a scene.
Without it, the AI can only guess at image contents based on filenames. With previews,
it can make informed decisions about which sprite, texture, or icon to use.

## How It Works

The AI calls `preview_asset` with an array of `res://` image paths. For each path:

1. Loads the image via `Image::load()`
2. Optionally crops to a `region` (source pixels), then scales per the `scale` knob
3. Encodes as PNG, then base64
4. Returns the preview as a vision image the AI can see directly

Multiple images can be previewed in a single tool call (batch mode).

### Resolution Budget, Region, and Scale

Previews never exceed source resolution — the tool never upscales. Detail is
self-serve through cropping:

- **First call**: no region — an overview of the whole image. In `auto` mode it
  passes through at native resolution when it fits the `max_size` budget
  (default 1024px, longest edge), and is downscaled to fit otherwise. Most
  pixel-art images pass through untouched. The 1024 default reflects acceptable
  per-preview cost (roughly 1,000-1,500 tokens per image) and the useful
  resolution ceiling of most vision models.
- **Zoom call**: pass `region` `[x, y, w, h]` in source pixels to preview only
  that rect (e.g. one tile of a tilesheet) — a small region comes back at
  native 1:1 pixels.
- **`scale`**: `"auto"` (default) as above. Explicit divisors bypass the budget
  entirely: `"1x"` = exact source resolution, `"2x"` = half, `"4x"` = quarter.
  This is the deliberate "I accept the context cost" lane — `1x` on a 4K image
  returns the full 4K image, so it should be reserved for genuine need (prefer
  region + `1x`).

The budget only governs `auto` mode. This replaces the observed workaround of
the model shelling out to external image tools to crop regions itself.

### Reported Dimensions

Each preview entry reports the file's true dimensions (`source_width` /
`source_height`), the source rect actually shown (`region`, echoed even for
whole-image previews and clamped to the image bounds), the preview's dimensions
(`preview_width` / `preview_height`), and `effective_scale` — preview pixels per
source pixel (always at most 1.0; 1.0 = native 1:1, below 1 downscaled). The
model maps a preview pixel back to the source with
`source_x = region[0] + preview_x / effective_scale` — no other arithmetic. The
distinction between source and preview dimensions matters: the model must base
layout and scale math on the source dimensions, never the preview. (Earlier
versions reported only the post-resize size, which led models to treat a 256px
thumbnail as the asset's real resolution and mis-derive scale factors from it;
later, models re-derived crop-to-source coordinate mappings by hand, which
`effective_scale` now eliminates.)

### Supported Formats

`.png`, `.jpg`, `.jpeg`, `.bmp`, `.tga`, `.webp`, `.svg`

### Parameters

- `paths` (required, array of strings): Image file paths (`res://...`)
- `max_size` (optional, int): Auto-mode budget — max preview dimension in
  pixels (default 1024, range 32-2048); explicit scale divisors bypass it
- `region` (optional, `[x, y, w, h]` in source pixels): Crop to this rect;
  applies to every path in the call
- `scale` (optional, string): `"auto"` (default), or an uncapped divisor like
  `"1x"`, `"2x"`, `"4x"`

### Typical Workflow

1. AI calls `list_files` to see what assets exist
2. AI calls `preview_asset` on image files it wants to inspect (overview)
3. AI zooms into a `region` where fine detail matters (e.g. reading individual
   tiles in a tileset), getting native-resolution pixels for small rects
4. AI uses the visual information to choose the right asset for the task

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
