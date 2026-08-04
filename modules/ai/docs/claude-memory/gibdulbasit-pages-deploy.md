---
name: gibdulbasit-pages-deploy
description: "How to deploy Godot web builds to the user's GitHub Pages site (gibdulbas.it)"
metadata: 
  node_type: memory
  type: reference
  originSessionId: c7e1cd4b-8bca-4ff4-8ca5-8259fc39e63b
  modified: 2026-07-23T22:12:54.768Z
---

GitHub Pages site: repo `Mspana/gibdulbas.it`, cloned at `C:\Users\Matthew\Documents\gibdulbasit`, branch `main`, custom domain `gibdulbas.it` (CNAME at root). Convention: each page/game lives in a subfolder (e.g. `harry/`, `a-personal-vibe/`).

Deploy a Godot web build: headless export with the fork's editor, naming the output `index.html` so files come out as `index.*`:
`./bin/godot.windows.editor.x86_64.console.exe --headless --path <project> --export-release "Web" <outdir>/index.html`
Then copy `index.*` into a subfolder, commit, push. Site updates in under a minute.

Game deployed 7/2026: "a personal vibe" (`C:\Users\Matthew\Documents\a-personal-vibe`) at https://gibdulbas.it/a-personal-vibe/. Preset uses threads + PWA (COI headers via service worker, needed since Pages can't send COOP/COEP).

Gotcha (cost hours 7/23/2026): if the export preset is in "selected resources" mode (`export_filter="resources"`) with an empty `export_files` list, the pck silently exports nearly empty (autoloads + their preloads only) — no error anywhere. Check `export_filter="all_resources"` in `export_presets.cfg` before exporting. Verify pck completeness by parsing its index (GDPC header, file count) rather than trusting a clean export log. See also [[ai-chat-log-locations]].
