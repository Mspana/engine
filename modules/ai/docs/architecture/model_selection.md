# Model Selection

The AI panel dropdown lists individual models rather than providers. Each model maps
to a provider backend (xAI, OpenAI, Anthropic, Gemini) that handles API communication.
Users pick the model they want; the provider is resolved automatically.

## Available Models

| Model | Provider | Vision | Context |
|---|---|---|---|
| Claude Sonnet 4 | Anthropic | Yes | 200k |
| Gemini 3 Flash | Gemini | Yes | 1M |
| Gemini 3.1 Flash Lite | Gemini | Yes | 1M |
| Gemini 3.1 Pro | Gemini | Yes | 1M |
| GPT-4o Mini | OpenAI | Yes | 128k |
| Grok 4 | xAI | Yes | 128k |
| Grok 4 Fast | xAI | Yes | 128k |
| Kimi K2.6 (Moonshot) | Moonshot | Yes | 256k |
| Kimi K3 (Moonshot) | Moonshot | Yes | 1M |

Default: Grok 4 Fast. The selection persists per-project via EditorSettings project
metadata and restores on restart.

## How It Works

A static model catalog (`AIProvider::get_available_models()`) returns the full list.
The dropdown populates from it alphabetically. On selection:

1. Look up the provider identifier from the catalog
2. Instantiate the correct provider class
3. Call `set_model()` with the chosen model ID
4. Set the provider on the AI singleton
5. Save to `EditorSettings::set_project_metadata("ai", "selected_model", ...)`

## Adding a New Model

1. Add an entry to `get_available_models()` in `ai_provider.cpp`
2. Add the model to `get_context_window_tokens()` and `model_supports_vision()`
3. The dropdown picks it up automatically

## Gemini Thought Signatures

Gemini 3.x models attach an opaque `thoughtSignature` to function call responses.
This must be echoed back in subsequent requests or the API returns a 400. The Gemini
provider extracts the signature from responses and echoes it when building the next
request. Other providers ignore it.

Swapping to a Gemini 3.x model mid-conversation (after another model made tool calls
without signatures) may fail. Starting a new chat after switching avoids this.

## Model Logging

Each time the user sends a message, a `model_info` item is written to the chat JSONL
before the run starts. It records `model_id` and `provider` so external tools can tell
which model handled each turn. See [Chat Logging](chat_logging.md) for the item format.

## Key Files

- `modules/ai/ai_provider.h` -- `ModelEntry` struct, `get_available_models()`
- `modules/ai/ai_provider.cpp` -- model catalog, context/vision metadata
- `modules/ai/editor/ai_status_indicator.cpp` -- dropdown UI and selection handler
