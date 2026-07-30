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

## Step 4 — Provider credentials (secrets — never committed)

AI provider connectivity does NOT travel with the repo. No system env vars
are involved; everything is file-based:

- **`modules/ai/.env`** (gitignored) holds the provider API keys as
  `KEY=VALUE` lines. Copy it from the old machine via a secure channel, or
  recreate it with whichever of these the engine reads (set only the ones
  in use): `ANTHROPIC_API_KEY`, `OPENAI_API_KEY`, `GEMINI_API_KEY`,
  `XAI_API_KEY`, `MOONSHOT_API_KEY`, `DEEPINFRA_API_KEY`,
  `PARASAIL_API_KEY`, `CLARIFAI_API_KEY`.
  Lookup order (`AIProvider::load_api_key_from_env`): system env var, then
  `.env` in `bin/`, engine root, or `modules/ai/`.
- **Codex harness auth**: `modules/ai/harness_spike/codex_home/` is
  gitignored except `config.toml`. Copy `auth.json` from the old machine to
  restore the codex login — or copy the entire folder to also keep the
  harness agent's sessions/memories/goals state (the `*.sqlite` files).

An agent running this setup should create `modules/ai/.env` with the key
names above and empty values, then ask the user to fill them in — never ask
for the secrets in chat.

## Step 5 — Codex harness binary (not in git)

The codex harness (`CodexHarnessDriver::start_session`) spawns a pinned
codex CLI from `modules/ai/harness_spike/bin/codex-x86_64-pc-windows-msvc.exe`
unless `ARISTOTLE_CODEX_EXE` points elsewhere. That whole `bin/` directory
is gitignored — the main exe is ~340 MB, over GitHub's file-size limit — so
on a new machine, restore it one of these ways:

1. Copy `modules/ai/harness_spike/bin/` from the old machine (bring the
   helper exes too: `codex-command-runner.exe`,
   `codex-windows-sandbox-setup.exe`).
2. Download codex-cli **0.145.0** for `x86_64-pc-windows-msvc` from the
   openai/codex GitHub releases and extract it there.
3. Install codex anywhere else (e.g. `npm i -g @openai/codex@0.145.0`) and
   set `ARISTOTLE_CODEX_EXE` to the binary's full path.

Keep the version pinned at **0.145.0** unless the harness's app-server
protocol handling has been revalidated against a newer codex.

## Gotchas

- Upstream Godot's `.gitignore` ignores `.vscode/`. The shipped files are
  force-added; any **new** file added under `.vscode/` needs `git add -f`.
- Keybindings cannot live at workspace level in VS Code/Cursor — that is why
  step 1 exists at all. Do not "fix" this by moving them into `.vscode/`.
