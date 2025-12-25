# AI Module for Godot 4

A custom Godot 4 module that integrates AI-powered scene manipulation. The AI receives natural language prompts and returns structured actions that modify the scene (create nodes, set properties, attach scripts, etc.) with full undo/redo support.

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                        GDScript / Editor                         │
│                     AI.request_actions(prompt)                   │
└─────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────┐
│                         AI Singleton                             │
│  - Manages provider                                              │
│  - Validates action JSON                                         │
│  - Dispatches to execution helpers                               │
│  - Integrates RetrievalIndex for context                        │
└─────────────────────────────────────────────────────────────────┘
                                  │
                    ┌─────────────┴─────────────┐
                    ▼                           ▼
┌──────────────────────────┐    ┌──────────────────────────────────┐
│      AIProvider          │    │        RetrievalIndex            │
│  (OpenAI/Gemini/XAI/     │    │  - Scans project files           │
│   Dummy)                 │    │  - Provides relevant context     │
│  - send_request()        │    │  - Keyword-based scoring         │
│  - Emits request_completed│   └──────────────────────────────────┘
└──────────────────────────┘
                    │
                    ▼
┌─────────────────────────────────────────────────────────────────┐
│                    Action Execution                              │
│  _execute_create_node()    _execute_set_property()              │
│  _execute_create_script()  _execute_update_script()             │
│  _execute_attach_script()  (more coming...)                     │
│                                                                  │
│  All wrapped in EditorUndoRedoManager for Ctrl+Z support        │
└─────────────────────────────────────────────────────────────────┘
```

## File Structure

| File | Purpose |
|------|---------|
| `ai.h` / `ai.cpp` | Main `AI` singleton - validation, dispatch, execution |
| `ai_provider.h` / `ai_provider.cpp` | Provider base class + OpenAI, Gemini, XAI, Dummy implementations |
| `retrieval.h` / `retrieval.cpp` | RAG-lite context retrieval from project files |
| `register_types.cpp` | Module registration, singleton setup |
| `config.py` | SCons build configuration |
| `SCsub` | SCons build script |
| `test_ai_providers.gd` | In-editor smoke tests (attach to a Node, run scene) |
| `test_endpoint_health.gd` | Headless API connectivity test |
| `API_KEYS.md` | Documentation for setting up API keys |

## Available Actions

Defined in `ai.cpp` as `ALLOWED_ACTIONS`:

| Action | Args | Status |
|--------|------|--------|
| `create_node` | `node_name`, `node_type`, `parent_path` (optional) | ✅ Implemented |
| `delete_node` | `node_path` | 🔲 Validation only |
| `set_property` | `node_path`, `property_name`, `value` | ✅ Implemented |
| `create_script` | `file_path`, `language`, `content` | ✅ Implemented |
| `update_script` | `file_path`, `patch` | ✅ Implemented |
| `attach_script` | `node_path`, `script_path` | ✅ Implemented |
| `connect_signal` | TBD | 🔲 Not implemented |
| `run_project` | TBD | 🔲 Not implemented |
| `list_nodes` | TBD | 🔲 Not implemented |
| `list_files` | TBD | 🔲 Not implemented |

## How to Add a New Action

### 1. Add to ALLOWED_ACTIONS (ai.cpp ~line 32)

```cpp
static const Vector<String> ALLOWED_ACTIONS = {
    "create_node","delete_node","set_property",
    "create_script","update_script","attach_script",
    "your_new_action",  // ← Add here
    "connect_signal","run_project","list_nodes","list_files"
};
```

### 2. Add Validation (ai.cpp in `_validate_command_dictionary`)

```cpp
} else if (action == "your_new_action") {
    if (!args.has("required_arg") || args["required_arg"].get_type() != Variant::STRING) {
        error_msg = "'your_new_action' requires string 'required_arg'.";
        return false;
    }
}
```

### 3. Declare Execution Helper (ai.h)

```cpp
private:
    // Execution helpers
    void _execute_create_node(const Dictionary &args);
    // ...
    void _execute_your_new_action(const Dictionary &args);  // ← Add here
```

### 4. Implement Execution Helper (ai.cpp)

Follow this pattern:

```cpp
void AI::_execute_your_new_action(const Dictionary &args) {
    // 1. Get singletons
    EditorInterface *ei = EditorInterface::get_singleton();
    if (!ei) {
        ERR_PRINT("AI Execute: EditorInterface singleton not found.");
        return;
    }
    
    EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
    if (!undo_redo) {
        ERR_PRINT("AI Execute: EditorUndoRedoManager singleton not found.");
        return;
    }
    
    // 2. Get edited scene root
    Node *edited_scene_root = ei->get_edited_scene_root();
    if (!edited_scene_root) {
        ERR_PRINT("AI Execute 'your_new_action': No edited scene root.");
        return;
    }
    
    // 3. Extract and validate args
    String my_arg = args["required_arg"];
    
    // 4. Do your logic with null checks
    // ...
    
    // 5. Wrap in UndoRedo
    undo_redo->create_action("AI Your New Action");
    undo_redo->add_do_method(...);
    undo_redo->add_undo_method(...);
    undo_redo->commit_action();
    
    // 6. Log
    print_verbose(vformat("AIHelper: your_new_action ..."));
    print_line(vformat("AI: Executed your_new_action. ..."));
}
```

### 5. Add Dispatch (ai.cpp in `_process_and_execute_actions`)

```cpp
} else if (action_name == "your_new_action") {
    _execute_your_new_action(action_args);
}
```

### 6. Add Tests (test_ai_providers.gd in `test_validation()`)

```gdscript
# Test valid your_new_action command
var valid_json = '{"action": "your_new_action", "args": {"required_arg": "value"}}'
result = AI.validate_command_json(valid_json)
if result["valid"] == true:
    print("  ✓ Valid your_new_action command passed validation")
else:
    print("  ✗ Valid your_new_action command failed: " + result["error"])
    all_passed = false
```

## Providers

### Provider Hierarchy

```
AIProvider (base)
├── OpenAIProvider   - GPT-4, GPT-4o-mini, etc.
├── GeminiProvider   - Google Gemini Pro
├── XAIProvider      - Grok (x.ai)
└── DummyProvider    - Returns canned responses for testing
```

### Key Provider Methods

| Method | Purpose |
|--------|---------|
| `send_request(prompt, context)` | Initiates async API call |
| `build_request_body()` | Constructs provider-specific JSON payload |
| `parse_response()` | Extracts action JSON from API response |
| `get_system_prompt()` | Returns the system prompt explaining action format |

### Setting a Provider

```gdscript
# From GDScript
var openai = OpenAIProvider.new()
openai.api_key = "sk-..."
openai.model = "gpt-4o-mini"
AI.provider = openai

# Or use environment variables (auto-loaded)
# Set OPENAI_API_KEY, GEMINI_API_KEY, or XAI_API_KEY
```

### Default Provider

The module defaults to `XAIProvider` (Grok). See `AI::AI()` constructor in `ai.cpp`.

## Testing

### In-Editor Tests (test_ai_providers.gd)

Tests provider creation, configuration, validation, and switching.

**To run:**
1. Open Godot editor with your built engine
2. Create a scene with a Node
3. Attach `modules/ai/test_ai_providers.gd`
4. Run the scene (F6)
5. Check Output panel

**Cannot run headless** because:
- Extends `Node` (needs scene tree)
- Uses `AI` singleton (editor-only)
- Uses provider classes (editor module)

### API Connectivity Tests (test_endpoint_health.gd)

Tests if API endpoints are reachable and validates API keys.

**To run:**
```bash
./bin/godot.windows.editor.x86_64.exe --headless --script modules/ai/test_endpoint_health.gd
```

**Can run headless** because:
- Extends `SceneTree`
- Only uses core HTTP classes
- No editor dependencies

## API Keys

Set via environment variables or `.env` file in project root:

```bash
OPENAI_API_KEY=sk-...
GEMINI_API_KEY=...
XAI_API_KEY=xai-...
```

See `API_KEYS.md` for detailed setup instructions.

## UndoRedo Pattern

All actions use `EditorUndoRedoManager` for full undo/redo support:

```cpp
EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();

undo_redo->create_action("AI Action Name");

// For node operations
undo_redo->add_do_method(parent, "add_child", node, true);
undo_redo->add_do_method(node, "set_owner", scene_root);
undo_redo->add_do_reference(node);  // Keep reference alive
undo_redo->add_undo_method(parent, "remove_child", node);
undo_redo->add_undo_method(node, "queue_free");

// For property operations
Variant old_value = target->get(property_name);
undo_redo->add_do_method(target, "set", property_name, new_value);
undo_redo->add_undo_method(target, "set", property_name, old_value);

undo_redo->commit_action();  // Executes do_methods immediately
```

## Request Flow

1. **GDScript calls** `AI.request_actions("Create a Player node")`
2. **AI singleton** retrieves context via `RetrievalIndex`
3. **Provider** sends prompt + context to API
4. **API returns** JSON array of actions
5. **AI validates** each action against schema
6. **AI executes** valid actions via `_execute_*` helpers
7. **UndoRedo** wraps each action for Ctrl+Z support

## Building

The module is built as part of the Godot engine:

```bash
# From engine root
python -m SCons platform=windows target=editor
```

Module is configured via `config.py` and `SCsub`.

