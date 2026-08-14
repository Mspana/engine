# Screenshot Capture

Three capture tools are available to the AI, each suited to a different situation:

| Tool | What it captures | Sync? | When to use |
|---|---|---|---|
| `run_and_screenshot` | The running game window (main scene by default, or any scene via `scene_path`) | Async (launches + waits + stops game) | Verify real runtime behavior, physics, scripts, animations. |
| `capture_2d_viewport` | The 2D editor canvas (shared scene SubViewport, with current pan/zoom; editor gizmos excluded by default) | Sync | Quick visual check of the 2D scene the user is editing without running anything. |
| `capture_3d_viewport` | The last-used 3D editor viewport (its own SubViewport + camera) | Sync | Quick visual check of the 3D scene from the current editor camera angle. |

The editor viewport captures are dramatically cheaper than running the game, and they
show *exactly what the user sees* in the editor — including pan/zoom in 2D or the user's
current camera orbit in 3D. Prefer them for any visual question that doesn't need runtime
behavior. Fall back to `run_and_screenshot` when you need to see the game actually running.

## Running Game Capture (`run_and_screenshot`)

The manual F2 button and the `run_and_screenshot` tool both use
`DisplayServer::window_get_image_from_pid()` as the primary capture method. This calls
Win32 `PrintWindow(HWND, HDC, PW_RENDERFULL_CONTENT)`, which asks the game window to paint
itself into a bitmap. Unlike the previous screen-coordinate `BitBlt` approach, this captures
the actual window content even when occluded, unfocused, or partially offscreen.

### Pipeline

1. Orchestrator timer fires → `AI::trigger_game_screenshot()`
2. AI singleton → `GameView::request_ai_screenshot()` (or signal fallback)
3. GameView defers → `_do_ai_screenshot_capture()`
4. Gets embedded PID via `embedded_process->get_embedded_pid()`
5. Calls `DisplayServer::window_get_image_from_pid(pid)` — PrintWindow capture
6. Falls back to `screen_get_image_rect()` then `screen_get_image()` if PrintWindow fails
7. Encodes to base64 PNG → `AI::deliver_game_screenshot(b64)`

### Fallback Chain

1. **PrintWindow** via `window_get_image_from_pid(pid)` — captures window content directly
2. **Screen rect** via `screen_get_image_rect(rect)` — captures screen pixels at window coords
3. **Full screen** via `screen_get_image()` — captures entire monitor

Non-Windows platforms skip step 1 (base class returns empty) and use the screen-based
fallbacks.

### Scene Selection (`scene_path`)

Both `run_project` and `run_and_screenshot` accept an optional `scene_path`
(res:// path). When provided, the scene is launched via
`EditorRunBar::play_custom_scene` instead of the main-scene run command — so the
AI can test a specific scene without touching the project's
`application/run/main_scene` setting. The path is validated up front (must be a
`res://` path to an existing file); an invalid path fails the tool immediately
rather than letting the run_and_screenshot state machine poll until its 8-second
start timeout. The result reports which scene was actually run: `scene_path`
when a custom scene was requested, `main_scene` otherwise.

### Result Fields

On both success and error paths, the tool result includes `elapsed_to_screenshot_ms` —
wall-clock milliseconds from tool invocation to the terminal state (capture delivered or
phase timeout). The AI uses this to distinguish an immediate crash (elapsed ≈ 8000 ms =
POLL_START timeout, game never reported running) from a normal run (elapsed ≈
`wait_seconds` × 1000 + capture overhead) from a capture hang (elapsed ≈ `wait_seconds` ×
1000 + 10 000 = AWAIT_CAPTURE timeout). The underlying timestamp is captured once at action
entry into `_async_rns_action_start_ms` and never reset on phase transitions (unlike
`_async_rns_phase_start_ms`, which tracks per-phase elapsed).

### Key Files

- `platform/windows/display_server_windows.cpp` — `window_get_image_from_pid()` implementation
- `editor/plugins/game_view_plugin.cpp` — `_do_ai_screenshot_capture()`, `_on_ai_screenshot_pressed()`
- `modules/ai/ai.cpp` — `trigger_game_screenshot()`, `deliver_game_screenshot()`
- `modules/ai/agentic_orchestrator.cpp` — `run_and_screenshot` async state machine

### Background

The game runs as a separate process, embedded into the editor via `EmbeddedProcess`. Its
rendering happens in Vulkan/D3D, not accessible via the editor's viewport system. Earlier
attempts to call `screen_get_image` from timer callbacks failed entirely (GDI can't
composite hardware-accelerated windows in that context). See
`docs/archive/screenshot_approaches.md` for the full history.

## Editor Viewport Capture (`capture_2d_viewport`, `capture_3d_viewport`)

The editor viewport captures bypass the running-game pipeline entirely. They read the
editor's own `SubViewport` textures directly on the main thread — no process launch,
no window capture, no PrintWindow. The operation is synchronous from the model's
perspective: the tool returns its result (with the PNG attached) in the same turn.

### Pipeline

1. Orchestrator dispatches `capture_2d_viewport` / `capture_3d_viewport` via
   `AI::execute_single_action` (normal sync path, not the async `run_and_screenshot`
   state machine).
2. Action resolves the target SubViewport:
   - **2D:** `EditorNode::get_scene_root()` — the shared edited-scene SubViewport that
     the 2D canvas renders. Reflects the user's current pan/zoom.
   - **3D:** `Node3DEditor::get_singleton()->get_last_used_viewport()->get_viewport_node()` —
     the SubViewport owned by the specific `Node3DEditorViewport` the user last
     interacted with, rendered through that viewport's own `Camera3D`.
3. Grabs the texture: `viewport->get_texture()->get_image()`.
4. Encodes PNG + base64 using the same helpers as `run_and_screenshot`
   (`Image::save_png_to_buffer()` → `CoreBind::Marshalls::raw_to_base64()`).
5. Returns the result dict with `screenshot_b64` inside `result` — the same shape
   `run_and_screenshot` uses. From there the shared image pipeline takes over
   (orchestrator strip → per-provider wire translation → UI pill → on-disk
   `<call_id>.png`). See [tool_result_images.md](tool_result_images.md) for the
   full pipeline.

### Cold-Start / Hidden-Tab Render

Before reading the texture, the helper runs a small synchronous render pass so
captures work even when the user has never clicked into the corresponding editor
panel — fresh project, or any session spent entirely on the Script tab.

The 3D editor viewport keeps rendering continuously via `Node3DEditorViewport`'s
`_process` (spatial gizmos, grid, camera, etc.), so 3D captures need only the
forced render. The 2D case has an additional wrinkle.

#### The 2D-disabled trap

`EditorNode` calls `RS::viewport_set_disable_2d(scene_root, true)` at startup
([editor_node.cpp:805](../../../editor/editor_node.cpp#L805)) and only flips it
to `false` while the 2D editor is the active main screen
([canvas_item_editor_plugin.cpp:5804](../../../editor/plugins/canvas_item_editor_plugin.cpp#L5804)).
With 2D rendering disabled at the RS level, forcing a render still produces just
the clear color — a grey image. So `exec_capture_2d_viewport` checks whether the
2D editor is currently visible: if not, it temporarily re-enables 2D and the
viewport environment for the duration of the capture, then restores both.

#### The 2×2 size trap

`scene_root`'s size is driven by the `SubViewportContainer` that hosts the 2D
canvas. On a cold start where the user has never opened the 2D tab, that
container has never run a layout pass — so `scene_root` still has the default
2×2 size, and the capture would return a 2×2 image. Fix: when the 2D tab is
hidden and the current size is too small, temporarily resize `scene_root` to
the project's `display/window/size/viewport_{width,height}` (or 1280×720 as a
fallback), capture, then restore.

This must use `SubViewport::set_size_force()`, not `set_size()`. The container's
`stretch=true` causes regular `set_size()` to be silently rejected with a
warning — `set_size_force()` is the public-in-C++ bypass intended for exactly
this case (used by `SubViewportContainer` itself when it lays out its child).

#### Render steps

Inside `_capture_subviewport_to_result`:

1. **Flush the MessageQueue.** `MessageQueue::get_singleton()->flush()` forces any
   pending deferred `CanvasItem::_redraw_callback`s to run now. Those callbacks
   submit the actual draw commands (CanvasItem `_draw()` → RS commands). Side
   effect to be aware of: if the 2D tab is visible and its overlay Control has a
   redraw queued, this flush runs `CanvasItemEditor::_draw_viewport`, which
   resets `scene_root`'s global canvas transform to the editor's own zoom/pan.
2. **Apply the caller's framing transform (if any).** The `frame_rect` transform
   is passed into `_force_render_subviewport` and applied *here* — after the
   flush, never before it. See "Why the transform is applied after the flush"
   below.
3. **Set the target SubViewport to `UPDATE_ONCE`.** Scene-level API, writes
   through to RS and keeps the Viewport node's cached state in sync.
4. **`RS::draw(false)`.** In threaded RS mode this only enqueues the draw; in
   non-threaded it renders immediately.
5. **`RS::sync()`.** Blocks until the render thread drains the queue, so
   `get_image()` sees the rendered texture and not a pre-render snapshot.
6. **Restore the previous `UpdateMode`.**

We do **not** deactivate the editor's root viewport. The preview plugins do that
because their target is an isolated off-screen viewport — our target is part of
the editor's live scene tree, and deactivating the root cancels the render we want.

We do **not** use the preview-plugin `frame_pre_draw` + semaphore path. That blocks
on a semaphore waiting for the render thread, which only works from a worker thread.
Our tool runs on the main thread, so that pattern deadlocks — the render thread
can't emit the signal until the main loop advances, and the main loop is blocked
in our `wait()`.

### Caveats of the Cold-Start Path

Things to know if you're touching this code or chasing a related bug:

- **Resize ripples.** `set_size_force()` triggers `NOTIFICATION_WM_SIZE_CHANGED`
  on `scene_root` and a layout pass on every `Control` child of the edited
  scene. The restore at the end of `exec_capture_2d_viewport` triggers a second
  one. User scripts hooked to `resized` will fire spuriously twice per
  hidden-tab capture. Acceptable today; revisit if a user reports stale-state
  bugs from `_notification(NOTIFICATION_WM_SIZE_CHANGED)` handlers.
- **`MessageQueue::flush()` runs every pending deferred call.** Not just the
  `_redraw_callback`s we queued — anything else queued via `call_deferred` from
  anywhere in the editor runs synchronously inside our tool call. Re-entrancy
  risk is non-zero.
- **`RS::draw(false)` renders all viewports.** Including the editor main and
  any other SubViewports. Causes a small visible hitch per capture.
- **`RS::sync()` has no timeout.** If the render thread wedges (driver hang,
  GPU timeout), the editor freezes identically to the deadlock we hit during
  development. No watchdog.
- **Tab-visibility check is a heuristic.** We use
  `CanvasItemEditor::is_visible_in_tree()` to infer whether 2D is the active
  main screen. That's the same condition `CanvasItemEditorPlugin::make_visible`
  uses today, but it's a coupling — if either side changes, our toggle/restore
  will desync.
- **Environment mode is force-restored to `DISABLED`.** If anything else ever
  sets it to `ENABLED` on `scene_root` while the 2D tab is hidden, we'd
  overwrite their state. Quiet assumption.

### Custom 2D Framing (`frame_rect`)

`capture_2d_viewport` accepts an optional `frame_rect: [x, y, width, height]`
argument (world-space units, matching node positions). When omitted, the
capture uses the editor's current pan/zoom — same as before. When present, the
capture frames the requested rect, fit-to-viewport with aspect ratio preserved
(may produce empty bands if the rect's aspect ratio differs from the
viewport's).

This works by overriding `scene_root`'s **global canvas transform** for the
duration of the capture. The 2D editor's own pan/zoom is implemented exactly
the same way — see `CanvasItemEditor::_draw_viewport` in
[canvas_item_editor_plugin.cpp:4046](../../../editor/plugins/canvas_item_editor_plugin.cpp#L4046),
which builds `scale(zoom) * translate(-view_offset)` from `zoom` and
`view_offset`. We use the same formula, derived from `frame_rect`:

```
zoom         = min(viewport_w / rect_w, viewport_h / rect_h)
center       = rect.position + rect.size / 2
view_offset  = center - viewport_size / (2 * zoom)
transform    = scale(zoom) * translate(-view_offset)
```

The override is *computed* after the hidden-tab `set_size_force` (so the framing
math sees the post-resize viewport size) but *applied* inside
`_force_render_subviewport`, after the MessageQueue flush and immediately before
the render. After the capture we restore the saved transform synchronously, then
call `CanvasItemEditor::update_viewport()` so the editor's `_draw_viewport`
re-syncs its own state on the next frame.

#### Why the transform is applied after the flush

`CanvasItemEditor::_draw_viewport`'s first act is to reset `scene_root`'s global
canvas transform from the editor's own `zoom`/`view_offset` — and the editor's
internal zoom includes the display scale (`EDSCALE`), so what the zoom widget
shows as "100%" is internally 2.0 on a 200%-scale display. An earlier version of
this tool set the framing transform up front and also queued an overlay redraw
via `update_viewport()` before capturing; the capture's own MessageQueue flush
then ran `_draw_viewport`, silently replacing the framing with the editor's view
whenever the 2D tab was the active main screen. Captures came out ~EDSCALE×
larger than requested and panned to the editor's current view — but only from
the 2D tab (hidden CanvasItems skip `NOTIFICATION_DRAW`, so captures taken from
the Script tab were correctly framed), which made the bug look like flaky
capture rather than a deterministic race.

Applying the framing after the flush closes the race regardless of the active
tab. The pre-capture `update_viewport()` call was removed entirely: the overlay
it redraws (grid, rulers, selection) lives on a Control *outside* `scene_root`
and can never appear in the capture anyway, so its only real effect was
triggering the clobber.

#### Pixel↔world metadata

Successful 2D captures include `px_per_world_unit` and
`world_rect: [x, y, width, height]` — the world-space region the image spans,
derived from the canvas transform the render actually used. Use these to convert
image-pixel measurements into world/node coordinates. This matters most for
no-`frame_rect` captures: the editor's view transform includes `EDSCALE`, so
image pixels are generally *not* 1:1 with world units.

#### Why no off-screen SubViewport

The cleaner alternative — stand up a sibling SubViewport sharing the
`World2D` and capture from it — would avoid mutating editor state. It's
unnecessary because the editor overlays (rulers, gizmos, grid, selection,
guides) are drawn on a sibling Control of the SubViewportContainer (the
`viewport` Control inside `CanvasItemEditor`), not on `scene_root` itself.
Capturing `scene_root` directly is already overlay-free, so the off-screen
route's main selling point disappears, and we avoid lifecycle code (creating,
parenting, sizing, destroying a temporary SubViewport per call).

The cost is a one-frame visual flicker if the user happens to be on the 2D
tab while the AI captures. For a tool call (already a heavyweight async
operation from the user's perspective) this is acceptable.

#### Caveats

- **No "fill" mode.** Only fit (preserves aspect ratio, may letterbox). If a
  user wants pixel-perfect framing, they have to match aspect ratios.
- **Restore is best-effort.** Between our `set_global_canvas_transform(saved)`
  and the editor's next `_draw_viewport` (deferred via `queue_redraw`),
  `scene_root`'s transform is the saved value — not necessarily the editor's
  *current* zoom/offset, if the user dragged something between save and
  restore. The editor will re-overwrite on its next redraw, so the window of
  divergence is one frame.
- **Validation is strict.** `frame_rect` must be a 4-element array of numbers
  with positive width and height. Anything else returns an `INVALID_ARGS`
  error and skips the capture entirely.

### Gizmo-Free Default (`include_gizmos`)

By default the 2D capture excludes editor gizmos. Two different things read as
"gizmos" in the editor: the CanvasItemEditor overlay (rulers, editor grid,
selection rectangles, the tile editor's grid) — which lives on a Control
outside `scene_root` and was never part of captures — and editor-only drawing
that 2D nodes emit *into the scene itself* when running under the editor:
collision shapes/polygons paint translucent debug fills, raycasts and
shapecasts draw arrows, `Camera2D` draws frame/limit rectangles, `Marker2D`
draws a cross, and the tile editor dims every `TileMapLayer` except the one
being edited. That second category shares the scene's canvas — an off-screen
SubViewport would render it identically — so it appeared in captures, and
models mistook it for scene content (teal collision fills were once debugged
as if they were sprite pixels).

For the capture frame, the tool hides the canvas items of node types whose
self-drawing is editor-only (they draw nothing in a running game) directly at
the RenderingServer level — the scene side sees no visibility change, no
notifications, no signals — and temporarily resets TileMapLayer highlight
dimming. Everything is restored immediately after the render, on success and
error paths alike. `include_gizmos: true` skips the suppression and captures
the editor helpers as before.

Caveat: a script that overrides `_draw()` on one of the suppressed node types
(e.g. custom drawing attached to a Path2D) is hidden with it in default
captures — use `include_gizmos: true` to see it.

### Custom 3D Framing (`shot_position` / `shot_target`)

`capture_3d_viewport` accepts optional shot parameters that let the AI shoot
the edited scene from an arbitrary viewpoint instead of the editor's current
orbit camera. When omitted, the tool behaves exactly as before (captures the
last-used `Node3DEditorViewport`). When present, it stands up a temporary
off-screen SubViewport that shares the edited scene's `World3D`, adds a
temporary `Camera3D` posed from the caller's arguments, renders one frame, and
tears down — without perturbing the editor's 4 viewports.

The parameters are named `shot_*` (not `camera_*`) to keep the tool's
inputs verbally distinct from `Camera3D` nodes in the scene. Models testing
this feature got confused when the tool was first shipped — they'd interpret
"move the camera" as editing a scene `Camera3D` instead of varying the tool
arguments, or reuse the same `camera_position` across retries. The `shot_*`
naming and the tool-description guidance ("these are inputs to this capture,
not tied to any Camera3D") are intended to head that off.

Parameters (all optional; `shot_position` and `shot_target` are
all-or-nothing):

- `shot_position: [x, y, z]` — world-space position the shot is taken from.
- `shot_target: [x, y, z]` — world-space point the shot looks at. Up vector
  is hardcoded `Vector3(0, 1, 0)`.
- `shot_fov: number` — vertical FOV in degrees (default `70`, open
  interval 0–180).
- `size: [w, h]` — output size (default `[1280, 720]`, min 64 per axis).
  Only applied on the custom-shot path; the default path returns the editor
  viewport's native size.

#### Shared-World3D approach

The 4 editor viewports never call `SubViewport::set_world_3d`, so
`Viewport::find_world_3d()` walks up to `SceneTree::get_root()->get_world_3d()`.
We explicitly give our temp SubViewport that same `World3D`:

```cpp
tv->set_world_3d(en->get_tree()->get_root()->get_world_3d());
// NOT set_use_own_world_3d(true) — we want to share, not clone.
```

The temp viewport renders the exact same lights, environment, and
`VisualInstance3D`s — no scene duplication, no state copy. The temp
Camera3D's `make_current()` is viewport-local (sets current camera on `tv`,
not on any editor viewport), so the user's editor view is untouched, and
any `Camera3D` the user has in the scene is likewise unaffected.

#### Gizmo exclusion via `cull_mask`

Editor gizmos (grid, origin axes, selection boxes, per-viewport move/rotate
gizmos, misc tools) are RenderingServer instances parented to the root
scenario and layer-filtered via `instance_set_layer_mask` on layers 24–30.
The editor's own cameras *opt in* to those layers via their `cull_mask`
(see [node_3d_editor_plugin.cpp:5581](../../../editor/plugins/node_3d_editor_plugin.cpp#L5581)).
Our temp camera uses `cull_mask = (1 << 20) - 1` (layers 0–19 only), which
naturally excludes every editor-layer category. The resulting image shows
scene geometry + lighting only.

This works *provided* no user scene geometry lives on layers 20+. Godot's
conventional 20 `VisualInstance3D` layers stay in 0–19, but user scripts
*can* place things higher — in that case those instances will not appear in
the capture. Acceptable default.

#### Render pipeline reuse

The temp SubViewport is parented under `EditorNode` for the duration of the
capture so it gets `NOTIFICATION_ENTER_TREE` and proper RS activation.
`_force_render_subviewport` (already used by 2D and by the default 3D path)
handles the single-frame render: MessageQueue flush → `UPDATE_ONCE` →
`RS::draw(false)` → `RS::sync()`. After the capture we `remove_child` then
`memdelete` — the camera is a child and is freed with the viewport.

One subtlety: `Node3D` dirties its transform on `ENTER_TREE` and queues a
**deferred** `NOTIFICATION_TRANSFORM_CHANGED` via
`SceneTree::xform_change_list`. Until that list is flushed,
`Camera3D::_update_camera` never runs, so the RS camera stays at identity —
the render would come back showing only the world environment (sky/ground,
no geometry), as if the camera were at the origin looking down -Z.
`MessageQueue::flush()` does *not* drain the transform list. The capture
calls `SceneTree::flush_transform_notifications()` right after
`make_current()` to force the pose into RS before draw. The default
editor-viewport path doesn't need this because its camera has been in the
tree for many frames and its RS transform is already current.

Result encoding (PNG + base64 in `screenshot_b64`) goes through the same
`_capture_subviewport_to_result` helper used everywhere else. The
orchestrator can't tell the difference between a custom-camera shot and a
regular one; it all flows through the same image pipeline.

#### Caveats

- **Editor-layer geometry is invisible.** See the gizmo discussion above —
  scene objects with a layer mask entirely above bit 19 will not render.
- **Gimbal at Y-parallel look directions.** `looking_at` with up=Y+ falls
  back to a secondary axis when the look direction is parallel to Y. A
  top-down shot should offset target slightly on X or Z
  (e.g. `shot_target: [0, 0, 0.01]`) to keep a stable orientation.
- **Near/far planes are fixed at 0.05 / 4000.** Matches the editor viewport
  defaults. Scenes spanning tens of thousands of units may see far-plane
  clipping; not exposed as a parameter (add if it ever matters).
- **Orthographic is not exposed.** Only perspective. Easy to add later.
- **Validation is strict.** Missing/mismatched pose, non-numeric arrays,
  equal `shot_position`/`shot_target`, FOV out of range, size below 64
  → `INVALID_ARGS` with no capture performed.

### Key Files

- `modules/ai/actions/project_actions.cpp` — `exec_capture_2d_viewport`,
  `exec_capture_3d_viewport`, shared `_capture_subviewport_to_result` helper
- `modules/ai/tools_array.inc` — tool declarations
- `modules/ai/ai.cpp` — dispatcher branches in `execute_single_action`

## If PrintWindow Returns Black Frames

`PW_RENDERFULL_CONTENT` (Windows 8.1+) should handle Vulkan/D3D windows. If it produces
black frames on specific hardware or driver configurations, two escalation paths exist:

- **Windows Graphics Capture API (WGC):** The modern Windows 10+ API using DXGI duplication.
  Same API OBS/ShareX use. Requires WinRT/COM interop (~200+ lines).
- **Game-side viewport capture:** Have the running game capture its own viewport via
  `get_viewport().get_texture().get_image()` and send PNG bytes back over the debugger
  protocol. Cross-platform, always accurate, but requires a new debugger message type.
