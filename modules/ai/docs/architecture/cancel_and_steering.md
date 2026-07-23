# Cancelling and Steering a Running Turn

The chat composer follows the interaction model popularized by Claude Code: stopping the AI is instant, and messages typed while it works steer the *current* run instead of waiting for it to finish.

## Instant Stop

Pressing **Stop** (or Escape in the composer) ends the run immediately. There is no "Stopping..." wait state: the button returns to **Send** at once and a new message can be sent right away.

What happens under the hood, conceptually:

- **Completed work is kept.** Everything the AI already did — tool calls that ran, scenes it changed — stays in the transcript.
- **Interrupted work is recorded, not orphaned.** Any tool calls the model requested but that never ran are answered with a synthetic "cancelled" result, so the transcript always remains valid for the next request.
- **The in-flight network request is abandoned, safely.** Each request carries a serial number; cancelling invalidates it. If the response arrives later anyway, it is recognized as stale and discarded — it can never leak into a run started after the cancel, even if the user immediately sent a new message.
- **The model is told.** A cancellation notice is kept in model-visible context so the next turn knows the previous one was interrupted and should verify state before continuing.

If the run was in the middle of `run_and_screenshot`, cancelling also stops the launched game.

## Steering Mid-Run

Typing a message and pressing Enter while the AI is working queues it as a **steering message**. It appears as a chip above the composer ("Will be read at the AI's next step") and is delivered to the model at its next decision point — right before its next request, after the current batch of tools finishes — without interrupting anything.

- When consumed, the chip becomes a normal user message in the transcript, at the position the model actually saw it.
- Until consumed, a queued message can be edited or removed (✕), and the model will never see it.
- If the run finishes before the message is consumed, it is sent as a fresh run, as before.
- Messages with attached images are not injected mid-run (injections are text-only); they wait and send as a fresh run so the images come along.

## Cancelling with Queued Messages

Cancelling is treated as "I'm reconsidering": any messages still queued (not yet read by the AI) are returned to the composer for editing rather than auto-starting a new run.
