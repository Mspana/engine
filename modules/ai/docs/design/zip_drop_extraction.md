# Zip Drop Extraction

## 1. Objective

When a user drags a `.zip` file from the OS into the FileSystem dock, ask what they want instead of silently copying the archive: a dialog offers **Extract to New Folder**, **Import as .zip**, or Cancel.

## 2. Why

Dropping a zip into a project almost always means "I want the files inside it" — an asset pack, a GitHub download, a batch of sprites. Previously the editor copied the archive verbatim, leaving the user to extract it externally and re-import. The zip itself is rarely the thing they want, but sometimes it is (e.g. exported packs), so the choice is explicit rather than automatic.

## 3. The Drop Pipeline

Same pipeline as documented in `gif_import_rfc.md`:

```
OS drop → Window "files_dropped" signal → EditorNode::_dropped_files()
        → EditorNode::_add_dropped_files_recursive() (plain dir->copy per file)
        → EditorFileSystem::scan_changes()
```

## 4. Design: Pre-Copy Interception

The zip check lives **inside `EditorNode::_dropped_files()`, before any copy happens**. Dropped files are partitioned: non-zips copy immediately exactly as before; zips are held in pending state (paths plus the destination folder, captured at drop time while the mouse position is still valid) and the dialog pops up. The archive never touches the project unless the user picks "Import as .zip".

This deliberately contrasts with the GIF import handler, which connects to `files_dropped` in parallel and runs *after* EditorNode has already copied the file, then converts and deletes it. The GIF flow is flagged in the backlog to migrate to this pre-copy pattern.

## 5. Dialog UX

- One dialog per drop; if several zips are dropped together, one answer applies to all of them. Non-zips in a mixed drop are unaffected.
- **Extract to New Folder** (default/Enter): each zip extracts into a new folder named after the archive (`foo.zip` → `foo/`), auto-renamed on collision (`foo (2)`, `foo (3)` — same convention as the FileSystem dock's duplicate handling).
- **Import as .zip**: the old behavior, a plain copy.
- **Cancel** / ESC: the drop is ignored entirely.

## 6. Extraction Details

`EditorNode::_extract_zip_to_dir(zip_path, dest_dir, r_failed_files)` — modeled on the asset installer's loop, with gaps fixed that both existing extraction sites (asset installer, project manager) have:

- **Single-root stripping**: if every entry lives under one common top folder, that folder is stripped so `foo.zip` containing `foo/...` doesn't produce `foo/foo/...`. Detection is segment-based, so it works for archives that omit explicit directory records.
- **`__MACOSX` entries are skipped** (macOS zip metadata).
- **Zip-slip guard**: entries whose resolved path would escape the destination folder (e.g. `../evil.txt`) are rejected and reported. Neither pre-existing extraction site guards against this.
- Unicode-safe entry names via `godot_unzip_get_current_file_info` (`core/io/zip_io.h`).
- A ProgressDialog tracks extraction; failures (including a corrupt/fake zip) are collected into one warning dialog. There is no fallback to copying — the user chose extraction.

The routine is deliberately state-free (absolute paths in, failure list out, no member access), so it can be promoted to a shared utility when the GIF handler — or any other drop-conversion flow — is reworked onto the interception pattern.

## 7. Scope Notes

- Only top-level dropped `.zip` *files* trigger the dialog. Zips nested inside a dropped folder copy as plain files, and a directory literally named `foo.zip` is treated as a folder.
- Drop targeting is unchanged: the hovered dock folder wins, falling back to the dock's current directory.
