## AI Module - Multi-Provider Support

The AI module now supports multiple AI providers including OpenAI, Google Gemini, and x.ai (Grok). The default provider has been set to **XAIProvider** using the `grok-4-fast` model. API keys are automatically loaded from system environment variables or a `.env` file located in the engine root or bin directory. Each provider implements asynchronous HTTP requests using Godot's `HTTPRequest` node, with proper error handling and signal-based callbacks. The system has been successfully tested with live API calls to Grok, demonstrating the ability to generate and execute actions (like node creation) based on natural language prompts. Provider classes are now registered with Godot's ClassDB, making them accessible from GDScript for easy runtime configuration.

## AI Module - RAG-Lite Retrieval System

The AI module now includes an in-process retrieval system that automatically enriches AI prompts with relevant project context. The **RetrievalIndex** class lazily scans the project directory on first use, indexing GDScript (`.gd`), C# (`.cs`), scene (`.tscn`), and resource (`.tres`) files. When `AI::request_actions()` is called, the system uses keyword-based scoring to find the top 8 most relevant files based on the user's prompt and active scene, adding them as context before sending to the AI provider. This enables the LLM to generate more accurate and project-aware code suggestions without requiring external dependencies or embedding models. The index builds once per editor session and includes smart file size limits (200KB per file) and snippet truncation (3KB per snippet) to maintain performance while providing meaningful context.

**Future Improvements:**
- Replace keyword-based scoring with embedding-based semantic similarity for better relevance matching
- Implement intelligent file chunking to split large files into meaningful segments (functions, classes, scenes) instead of storing entire files
- Add incremental index updates to detect file changes during the editor session without full rebuilds
- Persist the index to disk between sessions for faster startup times
- Make parameters configurable (top_k, file size limits, snippet length) via editor settings or project config
- Integrate with Godot's Language Server Protocol (LSP) for better code structure understanding and symbol-based retrieval
- Support additional file types (shaders, resources, documentation) and improve filtering of irrelevant files (e.g., generated files, third-party addons)
- Add caching layer to avoid re-scoring unchanged snippets on similar queries

## AI Helper - Headless Smoke Test

A minimal sample project (`sample_project/`) with a headless smoke test script has been added to validate engine functionality without requiring editor interaction. The test script extends `SceneTree` and can be executed with `--headless --script` parameters to perform automated validation. It loads the main scene from ProjectSettings, dynamically locates a Player node, simulates input actions (`ui_right` for 30 frames), and verifies that the game loop advances correctly by logging position changes. The test runs for 60 frames (~1 second), validates player movement, and exits cleanly with code 0 on success. All output is prefixed with "SMOKE:" for easy grep-filtering in CI/CD pipelines. The implementation is pure GDScript with no engine modifications, demonstrating proper usage of Godot 4's main loop API, input simulation via `Input.action_press()`, and scene tree manipulation for testing purposes.

**Usage:**
```bash
bin/godot.windows.editor.x86_64.exe --headless --path sample_project --script res://addons/ai_helper/headless_smoke_test.gd
```

