# AI Game Engine

## North Star

A user types a sentence like:

> **“Make the player jump higher”**

The editor updates the game automatically. The user presses **Play** and it works: **no debugging loops, no code spelunking**.

* The AI does *not* freely edit files
* It emits **structured actions**
* The engine **validates, executes, and (eventually) tests** those actions

---

## The 5 Levels of Project Understanding

These levels define what the AI can "see" and reason about. We are building them progressively.

---

Level 1 - Repository Knowledge - what files exist - *implemented*
Level 2 - Code Knowledge - what the files say - *implemented*
Level 3 - Game look - images of the running game
Level 4 - Game function - runtime behavior/logic
Level 5 - Game feel - interaction with the running game

---

## Engine Architecture Overview

### 1. Godot Fork + Editor Plugin

* Forked Godot 4 editor
* Custom `EditorPlugin` dock (AI Panel)
* UndoRedo‑wrapped mutations

---

### 2. Action Interface

The AI operates through an MCP server.

**Core Actions**

"- create_node: Create a new node in the scene tree\n"
"  Args: {\"node_name\": string, \"node_type\": string, \"parent_path\": string (optional)}\n"
"- delete_node: Delete a node from the scene tree\n"
"  Args: {\"node_path\": string}\n"
"- duplicate_node: Duplicate a node in the scene tree\n"
"  Args: {\"node_path\": string, \"new_name\": string (optional, defaults to \"<old_name>_copy\")}\n"
"- set_property: Set a property on an existing node\n"
"  Args: {\"node_path\": string, \"property_name\": string, \"value\": any}\n"
"- rename_node: Rename a node (prefer this over set_property for name changes)\n"
"  Args: {\"node_path\": string, \"new_name\": string}\n"
"- reparent_node: Move a node to a new parent\n"
"  Args: {\"node_path\": string, \"new_parent_path\": string, \"index\": int (optional)}\n"
"- create_script: Create a new script file (GDScript only)\n"
"  Args: {\"file_path\": string (e.g. \"res://scripts/Enemy.gd\"), \"language\": \"GDScript\", \"content\": string}\n"
"- update_script: Update an existing script file with new content\n"
"  Args: {\"file_path\": string, \"patch\": string (full file content)}\n"
"- attach_script: Attach a script to a node\n"
"  Args: {\"node_path\": string, \"script_path\": string}\n"
"- detach_script: Detach a script from a node\n"
"  Args: {\"node_path\": string}\n"
"- rename_script: Rename/move a script file\n"
"  Args: {\"old_path\": string, \"new_path\": string}\n"
"- delete_script: Delete a script file\n"
"  Args: {\"file_path\": string, \"detach_from_nodes\": bool (optional, default false)}\n"
"- connect_signal: Connect a signal from an emitter node to a target method\n"
"  Args: {\"emitter_path\": string, \"signal_name\": string (e.g. \"pressed\"), \"target_path\": string, \"method_name\": string (e.g. \"_on_button_pressed\"), \"binds\": array (optional), \"flags\": int (optional)}\n"
"- disconnect_signal: Disconnect a signal from an emitter node to a target method\n"
"  Args: {\"emitter_path\": string, \"signal_name\": string, \"target_path\": string, \"method_name\": string}\n"
"- create_scene: Create a new scene file\n"
"  Args: {\"scene_path\": string (e.g. \"res://scenes/Main.tscn\"), \"root_type\": string (optional, default \"Node\"), \"root_name\": string (optional, default \"Main\")}\n"
"- open_scene: Open a scene file in the editor\n"
"  Args: {\"scene_path\": string (e.g. \"res://scenes/Main.tscn\")}\n"
"- save_scene: Save the currently edited scene\n"
"  Args: {}\n"
"- close_scene: Close the current scene tab\n"
"  Args: {\"save_if_modified\": bool (optional, default true)}\n"
"- set_main_scene: Set the project's main scene in ProjectSettings\n"
"  Args: {\"scene_path\": string (e.g. \"res://scenes/Main.tscn\")}\n"
"- set_project_setting: Set a project setting value\n"
"  Args: {\"key\": string (e.g. \"display/window/size/viewport_width\"), \"value\": any}\n"
"- get_project_settings: Get project settings (read-only, useful for introspection before mutation)\n"
"  Args: {\"prefix\": string (optional, e.g. \"display/\"), \"keys\": array[string] (optional), \"include_defaults\": bool (optional, default false)}\n"
"- create_autoload_singleton: Create an autoload singleton entry in ProjectSettings\n"
"  Args: {\"name\": string (required), \"script_path\": string (required, e.g. \"res://scripts/GameManager.gd\"), \"enabled\": bool (optional, default true)}\n"
"- remove_autoload_singleton: Remove an autoload singleton entry from ProjectSettings\n"
"  Args: {\"name\": string (required)}\n"
"- import_asset: Import an asset file from OS path to project path (v0: copies bytes, import pipeline runs later)\n"
"  Args: {\"source_path\": string (required, absolute OS path), \"dest_path\": string (required, res://...), \"overwrite\": bool (optional, default false)}\n"
"- delete_asset: Delete an asset file from the project\n"
"  Args: {\"asset_path\": string (required, res://...)}\n"
"- run_project (alias: play_test): Run/play the project\n"
"  Args: {\"mode\": string (optional, default \"play\", supported: \"play\", \"headless_smoke\"), \"scene_path\": string (optional)}\n"
"- list_nodes: List nodes in the current scene tree\n"
"  Args: {\"root_path\": string (optional)}\n"
"- get_node_info: Get detailed information about a single node\n"
"  Args: {\"node_path\": string}\n"
"- find_nodes_by_type: Find all nodes of a specific type in the scene tree\n"
"  Args: {\"type_name\": string (e.g. \"CharacterBody2D\")}\n"
"- list_files: List files in a directory\n"
"  Args: {\"directory\": string (e.g. \"res://scripts\"), \"glob\": string (optional, e.g. \"*.gd\")}";

---

### 3. Validation Layer

Every AI response:

1. Parse JSON
2. Validate against `action_schema.json`
3. Reject malformed or unknown actions
4. Potentially send feedback to the AI, to fix their commands
5. *Maybe* **for in progress work** dynamically suggest updates to the system prompt to prevent malformed requests. 

---

### 4. Retrieval Layer (Levels 1–2)

* Python retrieval server (embeddings + vector store)
* C++ editor requests top‑K snippets
* Snippets injected under `Project context:`

Even with large context windows, **RAG is mandatory** for scale and relevance.

---

### 5. Multi‑Turn Agent Loop

The system is iterative:

1. User prompts agent
2. Agent thinks
3. Agent requests more information, or lists actions
4. Engine executes actions
5. Engine returns results or errors
6. AI revises or requests context

---

## Shipping Phases

### Phase 1 — Action Implementation

* “Make player faster”
* “Add jump”
* “Create enemy”

AI can execute actions corresponding to what the user wants.

---

### Phase 2 — Interactivity

User can converse with the AI through a modern chatbot interface.
- Easily send new request, and AI gets history
- Make new chat
- Can edit old messages
- Return to breakpoints
- Maintain multiple chats

---

## Guiding Principle

**People make games.**

The engine exists to keep creativity high and friction low.

## Important Red Line - NO AI CREATIVITY

The AI will not:
1. Make AI Art
2. Make AI Music
3. Make AI Assets
4. Or make any other creative aspects of the game.

The purpose of the AI is to make implementation easier so users can focus on the fun part: designing the game.