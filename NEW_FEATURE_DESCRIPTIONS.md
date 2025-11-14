## AI Module - Multi-Provider Support

The AI module now supports multiple AI providers including OpenAI, Google Gemini, and x.ai (Grok). The default provider has been set to **XAIProvider** using the `grok-4-fast` model. API keys are automatically loaded from system environment variables or a `.env` file located in the engine root or bin directory. Each provider implements asynchronous HTTP requests using Godot's `HTTPRequest` node, with proper error handling and signal-based callbacks. The system has been successfully tested with live API calls to Grok, demonstrating the ability to generate and execute actions (like node creation) based on natural language prompts. Provider classes are now registered with Godot's ClassDB, making them accessible from GDScript for easy runtime configuration.

The default `max_tokens` limit has been increased from 2000 to 8000 to prevent response truncation when AI providers return full script content in `update_script` actions. This ensures JSON responses remain valid and complete, especially when modifying larger files.

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

## AI Helper - Sample Project & Editor Plugin

A minimal sample project (`sample_project/`) has been created for Day 3 AI testing with a complete platformer setup. The project includes an **AI Helper EditorPlugin** (`addons/ai_helper/plugin.gd`) that adds a dock panel to the editor's top-right area, providing a text input field and "Request Actions" button for natural language interaction with the AI module. The plugin demonstrates proper integration between GDScript editor plugins and C++ engine modules via the `AI` singleton.

The sample project features a functional CharacterBody2D player with platformer movement (SPEED=200, JUMP_VELOCITY=-350) designed for AI modification testing. A headless smoke test script validates engine functionality without editor interaction, extending `SceneTree` for automated validation. The test loads scenes, simulates input, and verifies game loop advancement with exit code 0 on success. All output is prefixed with "SMOKE:" for CI/CD pipeline filtering.

**Usage:**
```bash
bin/godot.windows.editor.x86_64.exe --headless --path sample_project --script res://addons/ai_helper/headless_smoke_test.gd
```

