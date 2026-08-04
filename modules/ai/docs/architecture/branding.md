# Branding

The engine presents itself as **Aristotle**. This document describes where the
branding lives and what deliberately remains "Godot".

## Identity

- The display name comes from `version.py` (`name = "Aristotle"`). It flows into
  the editor and project manager window titles, the About dialog, the CLI
  banner, the Output panel startup line, and the Windows exe metadata.
- `short_name` stays `"godot"` on purpose: it determines the user data folders
  (`%APPDATA%\Godot`, `user://` roots). Changing it would orphan existing editor
  settings, project caches, and AI chat logs.

## Logo and derived assets

The source of truth is `modules/ai/aristotle logo.png` (brown circled-A monogram
plus "Aristotle" wordmark on cream; ink `#35221F`, cream `#FDF3E6`).

All derived art is produced by `misc/scripts/generate_aristotle_assets.py`,
which traces the bitmap into vector paths and rasterizes the rest. Run it again
if the logo ever changes. It covers:

- Boot splash (the splash itself is built straight from the logo by `main/SCsub`).
- Editor window/taskbar icon (`main/app_icon.png`) and the Windows executable
  icons (`platform/windows/*.ico`).
- Editor UI logos: About dialog and credits roll (`Logo.svg`), project manager
  title bar (`TitleBarLogo.svg`), Help menu icon (`Godot.svg`), project file
  icons (`GodotMonochrome.svg`, `GodotFile.svg`), and the icon written into
  every newly created project (`DefaultProjectIcon.svg`).
- Root repository art (`icon.*`, `logo.*`) and the web editor/export logo.

The icon files keep their original Godot names — the engine looks them up by
name, and renaming them would touch many call sites for no visual gain.

`modules/ai/icons/` can override most editor icons by filename, but the
default project icon is resolved by a first-match lookup, so the logo icons
above are replaced in place in `editor/icons/` instead.

## What intentionally stays "Godot"

- Legal attribution: copyright lines, license texts, and the About dialog's
  authors/donors/third-party tabs (required by the MIT license).
- Historical/compatibility text: Godot 3 to 4 migration dialogs and messages
  that refer to real Godot versions or file formats.
- Links to Godot documentation and the "Support Godot Development" menu item,
  which point at Godot's actual sites.
- Engine-internal identifiers: class names, `GodotPhysics`, C# namespaces,
  the `user://` directory name, and similar — renaming these breaks projects.
- The AI assistant's system prompt still says the engine is Godot-based so the
  model can apply its Godot API knowledge.

## Not yet swept (low priority)

macOS bundle icons (`.icns`), Android launcher icons and app label, Linux
desktop/MIME files, and the macOS document icons in `misc/dist/document_icons/`
still carry Godot art or names. They only matter when packaging for those
platforms.
