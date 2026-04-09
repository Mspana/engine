# Screenshot Capture

## How It Works

Both capture paths (manual F2 button and `run_and_screenshot` tool) use
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

## If PrintWindow Returns Black Frames

`PW_RENDERFULL_CONTENT` (Windows 8.1+) should handle Vulkan/D3D windows. If it produces
black frames on specific hardware or driver configurations, two escalation paths exist:

- **Windows Graphics Capture API (WGC):** The modern Windows 10+ API using DXGI duplication.
  Same API OBS/ShareX use. Requires WinRT/COM interop (~200+ lines).
- **Game-side viewport capture:** Have the running game capture its own viewport via
  `get_viewport().get_texture().get_image()` and send PNG bytes back over the debugger
  protocol. Cross-platform, always accurate, but requires a new debugger message type.
