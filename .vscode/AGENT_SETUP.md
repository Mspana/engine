# Dev Environment Setup (agent runbook)

Instructions for an AI agent (or a patient human) to finish wiring up this
repo's editor setup on a new machine. The repo ships everything it can in
`.vscode/`; the steps below cover the parts VS Code/Cursor only reads from
user-level files.

## What the setup provides

| Key | Action | Lives in |
| --- | --- | --- |
| F5 | Build (SCons) then launch the Godot editor | `.vscode/launch.json` + `tasks.json` — works on clone, no setup needed |
| F6 | Launch `bin\godot.windows.editor.x86_64.exe --editor` without building | user keybindings (step 1) |
| Shift+F6 | Terminate the F6 launch | user keybindings (step 1) |

F6/Shift+F6 are gated on the setting `workspaceKeybindings.godotF6`, so they
are inert outside workspaces that declare it. The committed
`.vscode/settings.json` declares it for direct folder opens; step 2 covers
multi-root workspaces.

## Step 1 — Merge keybindings into the user profile

Copy the entries from [`keybindings-reference.jsonc`](keybindings-reference.jsonc)
into the user-level keybindings file, preserving any existing entries and
skipping any that are already present (match on `key` + `command`):

- Cursor (Windows): `%APPDATA%\Cursor\User\keybindings.json`
- VS Code (Windows): `%APPDATA%\Code\User\keybindings.json`
- Cursor (macOS): `~/Library/Application Support/Cursor/User/keybindings.json`
- Cursor (Linux): `~/.config/Cursor/User/keybindings.json`

The file is JSON-with-comments containing a single top-level array; append the
new objects to that array.

## Step 2 — Multi-root workspaces only

If this repo is opened directly as a folder, skip this step.

If it is a folder inside a `.code-workspace` (multi-root) file, folder-level
window settings are ignored, so add the flag to the workspace file's
`settings` block:

```json
"settings": {
    "workspaceKeybindings.godotF6": true
}
```

## Step 3 — Verify

1. F6 should launch the editor from `bin\` (requires a prior build so the exe
   exists). Shift+F6 should kill it.
2. F5 should run the SCons build task, then launch. Requires Python + SCons
   (`python -m SCons`) and the MSVC toolchain on PATH.
3. If a just-added keybinding does not fire, run "Developer: Reload Window".

## Gotchas

- Upstream Godot's `.gitignore` ignores `.vscode/`. The shipped files are
  force-added; any **new** file added under `.vscode/` needs `git add -f`.
- Keybindings cannot live at workspace level in VS Code/Cursor — that is why
  step 1 exists at all. Do not "fix" this by moving them into `.vscode/`.
