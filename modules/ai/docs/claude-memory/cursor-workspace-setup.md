---
name: cursor-workspace-setup
description: "Engine is opened via a multi-root .code-workspace — window-level settings must go in that file, not folder .vscode/settings.json"
metadata: 
  node_type: memory
  type: project
  originSessionId: b51c9684-7877-4a0d-911b-53577975ff27
  modified: 2026-07-30T18:45:53.274Z
---

The user opens the engine in Cursor through `C:\Users\Matthew\Documents\engine.code-workspace`, a multi-root workspace (folders: engine, Godot AI Chats, godot-ai-dashboard). In multi-root mode, window-level settings in a folder's `.vscode/settings.json` are ignored — they must go in the `.code-workspace` file's `settings` block. This bit us when gating F6/Shift+F6 keybindings on `config.workspaceKeybindings.godotF6` (July 2026): the flag lives in both the committed `.vscode/settings.json` (works for direct folder opens / fresh clones) and the `.code-workspace` file (works for the user's daily setup). F5 is the stock Start Debugging key running launch.json; F6/Shift+F6 are user-profile keybindings documented in the committed `.vscode/keybindings-reference.jsonc`.
