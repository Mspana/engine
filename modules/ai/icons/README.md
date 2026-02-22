# Aristotle Icon Overrides

Place SVG icon files here to override editor icons.

## How it works

The Godot build system (`editor/icons/SCsub`) scans `modules/*/icons/` directories
for SVG files. Any SVG with the same name as an existing editor icon will replace it.

## Naming convention

Icon names must match exactly (case-sensitive, PascalCase):
- `Play.svg` → overrides the Play button icon
- `Stop.svg` → overrides the Stop button icon
- `Save.svg` → overrides the Save icon
- etc.

## Available icons to override

See `editor/icons/*.svg` for the full list of editor icon names.

## Priority

Same-name icons from modules override the defaults.
Icons are converted to SVGTexture at build time and support DPI scaling.
