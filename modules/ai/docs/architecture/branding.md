# Branding

The engine presents itself as **Aristotle**. This records the principles behind
the rebrand, its risks, and the knowledge needed to maintain it — not a
change-by-change history.

## Principles

- **Display identity is Aristotle; lineage is Godot.** Anything a user reads as
  "the product" — names, logos, window titles, installer, taskbar identity —
  says Aristotle. Anything that is legal attribution (copyright lines, license
  tabs, About credits) or a factual upstream reference (Godot 3→4 migration
  dialogs) keeps the Godot name: the MIT license requires the former, honesty
  the latter. Where a brand name added nothing ("Open Godot online
  documentation"), the string was neutralized instead ("Open online
  documentation").
- **Identifiers only change when the fallout is understood.** Class names, C#
  namespaces, and stored project-setting values (e.g. `GodotPhysics2D`) stay —
  renaming them breaks projects for zero visual gain. The identifiers we *did*
  change, and their fallout, are under Risks below.
- **All derived art regenerates from one source.** `modules/ai/aristotle
  logo.png` (ink `#35221F`, cream `#FDF3E6`) is the only hand-made asset;
  `misc/scripts/generate_aristotle_assets.py` traces and rasterizes everything
  else: editor logo icons, window/exe icons, splash, root repo art, web logo,
  installer/console masters, and the document-icon masters. Never hand-edit a
  generated asset — change the script and re-run it.
- **"Support Godot Development" stays** in the Help menu (upstream deserves the
  funnel). It may move somewhere less prominent later, but it does not get
  removed or rebranded.

## Identity and data directories

`version.py` is the identity root: `name = "Aristotle"` is the display name;
`short_name = "aristotle"` is the filesystem/OS identity. short_name determines
`%APPDATA%\Aristotle\` (and macOS/Linux analogues): editor settings, caches,
export templates, and every project's `user://` root under `app_userdata/`.

**Risk — data migration (one-time, after the short_name change).** Everything
previously under `%APPDATA%\Godot` is orphaned, not deleted. Migration:

1. Copy the tree: `robocopy %APPDATA%\Godot %APPDATA%\Aristotle /E`.
2. Delete the stale junctions inside `Documents\Godot AI Chats\` (`rmdir`
   each one — junctions delete without touching their targets).
   `_ensure_chat_junction` in `modules/ai/ai.cpp` **skips junctions that
   already exist**, so until the old ones are removed they silently point at
   the dead location and the godot-ai-dashboard watches stale data. The next
   editor run recreates them against the new path.

The `Documents\Godot AI Chats` folder name itself is a literal in ai.cpp and
deliberately keeps its name — the dashboard and existing habits depend on it.

**Risk — Windows identity.** The AppUserModelID changed from
`Godot.GodotEditor.*` to `Aristotle.AristotleEditor.*` (and `Aristotle.<app>`
for exported games): pinned taskbar icons regroup once after first launch. The
Inno Setup installer has a fresh AppId GUID so an Aristotle install can never
collide with, or hijack the uninstall entry of, a real Godot install.

## Versioning

Version numbers (4.5.dev) deliberately track the upstream Godot base so merges
stay sane. Consequences:

- The editor's update checker (`EngineUpdateLabel`, setting
  `network/connection/check_for_updates`, default "check newest" on dev
  builds) compares against Godot's release feed, so when online it nags about
  Godot releases we will never install. Short-term fix: set the editor setting
  to "Disable Update Checks" (offline mode also silences it). Long-term:
  repoint or remove `editor/engine_update_label.cpp` once Aristotle has its own
  release identity. Never "update" over an Aristotle install with an upstream
  installer — they are separate apps with separate data dirs as of this
  rebrand.

## Branded surfaces

Display name (window titles, About, CLI banner, exe metadata) flows from
`version.py`. Generated art covers: boot splash (built from the logo by
`main/SCsub`), window/taskbar icon, Windows exe + console icons and their
`misc/dist` master SVGs, editor UI logos (`Logo`, `TitleBarLogo`, `Godot`,
`GodotMonochrome`, `GodotFile`, `DefaultProjectIcon` — names kept because the
engine looks icons up by name), root repository art, web editor logo, and the
document-icon masters in `misc/dist/document_icons/` (paper + monogram + a
per-type accent bar). New projects receive the Aristotle icon via
`DefaultProjectIcon`; pre-existing projects each own a local `icon.svg` that
had to be replaced per project.

Note: `DefaultProjectIcon` must live in `editor/icons/` (not the
`modules/ai/icons/` override dir) because `get_default_project_icon()` takes
the first name match in the icon table, and module icons register last.

## Not yet swept (matters only when packaging for these platforms)

- macOS: `Godot.icns`/document `.icns` bundles and their `Info.plist` refs.
- Android: launcher mipmaps, "Godot Engine 4" editor app label, strings.xml.
- Linux: `.desktop`/MIME/appdata files, X11 `WM_CLASS`, Wayland
  `app_id "org.godotengine.Godot"`.
- Network/runtime identity: HTTP `User-Agent: GodotEngine/...`, OpenXR
  application/engine names.
