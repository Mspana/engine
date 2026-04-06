# run_and_screenshot: Capture Approaches Tried

## Approach 1: `delay_usec` + `screen_get_image` on action thread

```mermaid
sequenceDiagram
    participant O as Orchestrator
    participant OS as OS
    participant DS as DisplayServer
    O->>OS: delay_usec(2s)
    Note over O,OS: ❌ Actions run on main thread<br/>UI freezes for 2 full seconds
    O->>DS: screen_get_image()
    DS-->>O: empty (GDI fails)
```
Actions run on the **main thread** (via `call_deferred`), so `delay_usec` locks the entire editor.

---

## Approach 2: Semaphore + dispatched capture

```mermaid
sequenceDiagram
    participant O as Orchestrator
    participant S as Semaphore
    O->>S: wait()
    Note over O,S: ❌ Deadlock — deferred calls<br/>can't process while main thread blocks
```
`Semaphore::wait()` on the main thread prevents any deferred call from ever flushing — instant deadlock.

---

## Approach 3: SceneTree timer → `screen_get_image` from orchestrator

```mermaid
sequenceDiagram
    participant T as SceneTree Timer
    participant O as Orchestrator
    participant DS as DisplayServer
    T->>O: _run_and_screenshot_tick()
    O->>DS: screen_get_image()
    DS-->>O: ❌ empty
    Note over DS,O: GDI BitBlt fails for Vulkan/D3D<br/>windows from timer callback context
```
Non-blocking ✓, but `screen_get_image()` returns empty from within a SceneTree timer callback on Windows — GDI can't composite hardware-accelerated windows in that context.

---

## Approach 4: Signal → GameView (synchronous capture)

```mermaid
sequenceDiagram
    participant T as SceneTree Timer
    participant O as Orchestrator
    participant AI as AI Singleton
    participant GV as GameView
    participant DS as DisplayServer
    T->>O: tick() — wait elapsed
    O->>AI: trigger_game_screenshot()
    AI-->>GV: game_screenshot_requested (sync)
    GV->>DS: screen_get_image_rect()
    DS-->>GV: ❌ still empty
    Note over GV,DS: GameView runs synchronously<br/>inside the same timer callback stack —<br/>same context, same failure
    GV->>AI: deliver_game_screenshot("")
    AI-->>O: game_screenshot_ready("") → error
```
Routed through GameView's proven capture code, but the synchronous signal call still runs inside the timer callback's call stack — same context problem.

---

## Approach 5: Signal → GameView → `call_deferred` (current)

```mermaid
sequenceDiagram
    participant T as SceneTree Timer
    participant O as Orchestrator
    participant AI as AI Singleton
    participant GV as GameView
    participant MQ as MessageQueue
    T->>O: tick() — wait elapsed
    O->>AI: connect(game_screenshot_ready, ONE_SHOT)
    O->>AI: trigger_game_screenshot()
    AI-->>GV: game_screenshot_requested
    GV->>MQ: call_deferred(_do_capture)
    Note over MQ: ❓ _do_capture never fires?<br/>Or fires but signal not received?
    O->>T: schedule 10s timeout tick
    Note over T,O: ❌ timeout fires — signal never arrived
```
`call_deferred` should push `_do_ai_screenshot_capture` out of the timer context into the message queue flush phase. But debug logs show `_on_ai_screenshot_requested` **never even fires** — meaning GameView was never connected to `game_screenshot_requested`, or the connection was lost.

---

**Next step:** debug build prints `AI_DBG: NOTIFICATION_READY in GameView` at editor startup to confirm whether the signal connection is ever made.
