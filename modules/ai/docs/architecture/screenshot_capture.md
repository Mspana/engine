# Screenshot Capture

Three capture tools are available to the AI, each suited to a different situation:

| Tool | What it captures | Sync? | When to use |
|---|---|---|---|
| `run_and_screenshot` | The running game window (main scene) | Async (launches + waits + stops game) | Verify real runtime behavior, physics, scripts, animations. |
| `capture_2d_viewport` | The 2D editor canvas (shared scene SubViewport, with current pan/zoom) | Sync | Quick visual check of the 2D scene the user is editing without running anything. |
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

### Known Limitation: Hidden Tabs

If the 3D tab is not currently visible, its `SubViewport::update_mode` defaults to
`UPDATE_WHEN_VISIBLE`, so it may return a stale or empty image. Same in reverse for
2D. A future phase will toggle `update_mode` and force a one-frame render before
capturing, but for now the model should prefer the viewport the user is actually
looking at.

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
