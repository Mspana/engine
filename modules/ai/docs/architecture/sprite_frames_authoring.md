# SpriteFrames Authoring

## Purpose

The `create_sprite_frames` tool builds a fully-populated `SpriteFrames` resource —
multiple named animations, per-frame textures, durations, fps, looping — and assigns
it to an `AnimatedSprite2D`/`AnimatedSprite3D` in one declarative call.

Without it, the AI hand-rolled frame animation: GDScript coroutines swapping textures
on timers, or grids of per-pixel ColorRects. Frames are added to a SpriteFrames via
*methods* (`add_animation`, `add_frame`, …), not settable properties, so neither
`create_resource` nor `set_property` could populate one. This tool makes the idiomatic
Godot path the easy path.

## How It Works

The AI calls `create_sprite_frames` with an `animations` spec and a target. The handler:

1. Validates the whole spec structurally (unique names, fps > 0, frame shapes), collecting
   every problem into one error so the model can fix the spec in a single retry
2. Validates every texture path loads as a `Texture2D` — atomically, before anything mutates
3. Builds the SpriteFrames in memory (the constructor's auto-created `"default"` animation
   is removed so the resource holds exactly the requested set)
4. Assigns it to the node's `sprite_frames` property under `EditorUndoRedoManager`, so
   undo/redo, checkpoints, dirty-marking, and scene diffs all work

### Embedded vs. Saved

By default the resource is **embedded** in the scene (serialized as a `sub_resource`),
matching what the editor does when you create an AnimatedSprite2D by hand — and the
whole change is undoable.

With the optional `save_path` (`res://….tres`), the resource is instead saved to disk
(`ResourceSaver::save` + `take_over_path`, then an `EditorFileSystem::update_file` so it
appears in the FileSystem dock immediately) and the scene references it as an
`ext_resource`. The disk write itself is *not* undoable — undo restores the node's old
`sprite_frames` but leaves the `.tres` file on disk (same asymmetry as `create_scene` /
`create_script`). Existing files are never overwritten; the call fails and suggests
`delete_asset`.

### Replace Semantics

The call replaces any existing SpriteFrames on the node wholesale (undo restores the
previous resource). This keeps the tool declarative: to modify an animation set, the
model re-issues the full desired state rather than patching increments.

### Sprite Sheets

A frame may carry `region: [x, y, width, height]`, which wraps the texture in a fresh
`AtlasTexture` cropped to that rect — one per frame, never shared. This is the intended
path for sprite sheets (including sheets produced by the GIF import flow). A region
extending outside the texture is a warning, not an error.

### Target Validation

The node is validated through its property list — a `sprite_frames` slot whose
`PROPERTY_HINT_RESOURCE_TYPE` accepts `SpriteFrames` — rather than by class name. This
covers `AnimatedSprite2D`, `AnimatedSprite3D`, and scripted nodes exposing the same
contract. Both AnimatedSprite types auto-select the first available animation on
assignment, so the removed `"default"` animation can never leave a node in a broken
state. Playback on scene start is the node's `autoplay` property (set separately via
`set_property`).

### Parameters

- `animations` (required, array): each `{name (required, unique), fps (default 5.0),
  loop (default true), frames (required, non-empty)}`
- `frames` entries: `{texture (required, res:// path), duration (default 1.0, relative
  multiplier), region (optional [x,y,w,h])}`
- `node_path` (optional if `save_path` given): target node
- `save_path` (optional): standalone `.tres`/`.res` destination
- `scene_path` (optional): standard mutation scene targeting

### Typical Workflow

1. AI calls `list_files` / `preview_asset` to find and inspect frame textures
2. AI creates an `AnimatedSprite2D` via `create_node` (if one doesn't exist)
3. AI calls `create_sprite_frames` with all animations in one call
4. AI sets `autoplay` (and `position`, etc.) via `set_property`, then verifies with
   `run_and_screenshot`

### Key Files

- `modules/ai/actions/node_actions.cpp` — `exec_create_sprite_frames()` implementation
- `modules/ai/actions/node_actions.h` — declaration
- `modules/ai/tools_array.inc` — native tool schema
- `modules/ai/ai.cpp` — allow-list, arg validation, dispatch, scene-mutation registration
- `modules/ai/ai_provider.cpp` — legacy prompt catalog entry
- `modules/ai/system_prompt.inc` — native-prompt animation guidance (steers frame
  animation to this tool and away from script-driven texture swapping)

### Limitations

- Total frames are capped at 10,000 per call (warning above 1,000).
- No overwrite flag for `save_path`; existing files must be deleted first.
- `AtlasTexture.filter_clip` is left at its default; adjacent-frame bleeding on tightly
  packed sheets isn't yet addressed.
- Re-saving over a `.tres` referenced by *other* scenes updates them only on their next
  load (resource cache takeover).
- `AnimationPlayer` track authoring (property/method tracks, keyframes) is a separate
  future tool.
