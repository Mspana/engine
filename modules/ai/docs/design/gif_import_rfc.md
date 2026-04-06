# RFC: GIF Import Support

## 1. Objective

When a user drags a `.gif` file into the Godot editor (from Windows Explorer into the FileSystem dock), detect it, decode the frames, produce a horizontal sprite-sheet PNG, and place that PNG in the project — ideally instead of the raw GIF, not alongside it.

## 2. Why

Godot has no native GIF support. If a user drags a GIF into their project, nothing useful happens — the file is silently copied but can't be used as a texture, sprite, or anything else. For a game engine with 2D as a first-class citizen, animated sprites from GIFs is a common workflow that should just work.

## 3. How GIFs Are Currently (Not) Handled

Godot's editor filesystem (`EditorFileSystem`) maintains an index of all project files. When files are added, it scans them, checks their extension against a list of recognized types, and queues them for import if applicable.

`.gif` is **not a recognized extension**. The scan at `_scan_fs_changes()` in `editor_file_system.cpp` checks each file's extension against `valid_extensions` — GIF isn't in that set. The file gets copied to disk but is invisible to `EditorFileSystem`. It won't appear in the FileSystem dock, won't trigger import, and won't fire `filesystem_changed` for any listener that walks `EditorFileSystemDirectory`.

Our current `GIFImportHandler` listens for `filesystem_changed` and walks `EditorFileSystemDirectory` looking for `.gif` files. It never finds any, because they're never indexed.

## 4. How Regular Files Are Handled

The full drop-to-import pipeline for a recognized file (e.g., `.png`):

```
1. User drags file from OS onto editor window
2. DisplayServer delivers drop event → Window::_window_drop_files()
3. Window emits "files_dropped" signal with OS file paths
4. EditorNode::_dropped_files() receives signal
5. EditorNode::_add_dropped_files_recursive() copies files to project dir
6. EditorFileSystem::scan_changes() is called
7. _scan_fs_changes() detects new files by comparing directory mtimes
   - Checks extension against valid_extensions
   - Creates FileInfo, gets resource type via ResourceLoader
   - Queues ACTION_FILE_ADD and ACTION_FILE_TEST_REIMPORT
8. For importable files: finds matching EditorImportPlugin
9. EditorImportPlugin::import() runs, producing .import file + imported resource
10. EditorFileSystem emits "filesystem_changed" and "sources_changed"
11. FileSystem dock updates UI
```

Key actors and their locations:
- `Window::_window_drop_files()` — `scene/main/window.cpp:1819`
- `EditorNode::_dropped_files()` — `editor/editor_node.cpp:6143`
- `EditorNode::_add_dropped_files_recursive()` — `editor/editor_node.cpp:6155` (the actual `dir->copy()`)
- `EditorFileSystem::_scan_fs_changes()` — `editor/editor_file_system.cpp:1456`
- `EditorImportPlugin` — `editor/import/editor_import_plugin.h`

## 5. Simple Fix

**Replace `EditorFileSystemDirectory` walk with `DirAccess` walk.**

Instead of asking `EditorFileSystem` for GIF files (which it will never index), scan the project directory directly using `DirAccess`. This bypasses the recognition problem entirely.

```cpp
void GIFImportHandler::_collect_gif_files_via_dir(HashSet<String> &r_paths) {
    // Walk res:// with DirAccess, looking for *.gif
    Vector<String> stack;
    stack.push_back("res://");
    while (!stack.is_empty()) {
        String dir_path = stack[stack.size() - 1];
        stack.resize(stack.size() - 1);
        Ref<DirAccess> da = DirAccess::open(dir_path);
        if (!da.is_valid()) continue;
        da->list_dir_begin();
        String name = da->get_next();
        while (!name.is_empty()) {
            if (da->current_is_dir() && name != "." && name != ".." && !name.begins_with(".")) {
                stack.push_back(dir_path.path_join(name));
            } else if (name.get_extension().to_lower() == "gif") {
                r_paths.insert(dir_path.path_join(name));
            }
            name = da->get_next();
        }
    }
}
```

**Pros:** Works today, no engine changes, minimal diff.

**Cons:** The GIF is already copied to the project before we detect it. The dialog appears after the fact. If the user converts, both the `.gif` and `_sheet.png` exist. If they cancel, a useless `.gif` sits in their project. The `filesystem_changed` signal may not fire at all for an unrecognized file type, so we may need a timer-based poll or hook into `scan_changes()` completion differently.

## 6. Ideal Fix (Intercept)

Intercept the file **before** it's copied to the project. When the user drops a `.gif`, convert it to a sprite-sheet PNG in a temp location, then copy the PNG instead — the GIF never touches the project directory.

The interception point is `EditorNode::_add_dropped_files_recursive()` at `editor/editor_node.cpp:6155`. This method iterates dropped file paths and calls `dir->copy(from, to)` for each one. If we could hook in here, we'd:

1. Check if the file extension is `.gif`
2. Decode and convert to sprite-sheet PNG in a temp directory
3. Copy the PNG to the target location instead
4. Skip the original `.gif`

The result: only the PNG appears in the project. No cleanup needed. The user doesn't see a file they can't use.

## 7. Problems with Intercept

**No extension point exists.** `EditorNode::_dropped_files()` is a direct signal handler with no virtual methods, no filter chain, and no plugin hook. There is no `EditorPlugin` virtual for file drops. The call chain is:

```
Window "files_dropped" signal → EditorNode::_dropped_files() → copy → scan
```

All hardcoded. An `EditorPlugin` cannot participate.

**Connecting to `files_dropped` before EditorNode** is technically possible but fragile — signal connection order isn't guaranteed, and you'd need to suppress EditorNode's handler for the GIF files while letting it handle the rest. There's no clean way to do this.

**`EditorImportPlugin`** is the official extension point for custom file types, but it runs *after* the file is copied and produces `.import` + `.godot/imported/` artifacts — not a standalone PNG in the project tree. It's designed for Godot's resource import pipeline, not for file-format conversion.

## 8. Suggestions

### 8a. Engine change: drop filter callback

Add a virtual method to `EditorPlugin`:

```cpp
// editor/plugins/editor_plugin.h
virtual PackedStringArray filter_dropped_files(const PackedStringArray &p_files, const String &p_to_dir);
```

`EditorNode::_dropped_files()` calls this on every registered `EditorPlugin` before copying. Each plugin can:
- Return the list unchanged (pass-through)
- Remove entries it handled (e.g., converted `.gif` → `.png` and copied the PNG itself)
- Replace entries (swap a path for a converted path)

Implementation in `editor_node.cpp`:

```cpp
void EditorNode::_dropped_files(const Vector<String> &p_files) {
    String to_path = FileSystemDock::get_singleton()->get_folder_path_at_mouse_position();
    if (to_path.is_empty()) {
        to_path = FileSystemDock::get_singleton()->get_current_directory();
    }

    // Let plugins filter/transform dropped files
    PackedStringArray files;
    for (const String &f : p_files) files.push_back(f);

    for (int i = 0; i < editor_data.get_editor_plugin_count(); i++) {
        files = editor_data.get_editor_plugin(i)->filter_dropped_files(files, to_path);
    }

    Vector<String> filtered;
    for (int i = 0; i < files.size(); i++) filtered.push_back(files[i]);

    String to_abs = ProjectSettings::get_singleton()->globalize_path(to_path);
    _add_dropped_files_recursive(filtered, to_abs);
    EditorFileSystem::get_singleton()->scan_changes();
}
```

**Pros:** Clean, composable, any module can handle any file type. Other plugins could use this for `.psd`, `.aseprite`, `.svg` conversion, etc.

**Cons:** Engine-level change. Upstream Godot would need to accept it (or we maintain it as a fork patch).

### 8b. Engine change: lightweight pre-copy hook

If the full filter API is too much, a simpler signal:

```cpp
// In EditorNode::_dropped_files(), before copying:
emit_signal("files_dropping", p_files, to_path);
```

Plugins connect and can modify the project directory before the copy+scan. Less clean than 8a (plugins would need to do their own file operations and somehow tell EditorNode to skip certain files), but simpler to implement.

### 8c. Hybrid: simple fix now, engine change later

Ship the `DirAccess` scan fix (section 5) immediately — it makes the current GIF handler functional. Then implement 8a as a separate PR targeting the engine's plugin API. Once the drop filter exists, migrate `GIFImportHandler` to use it and remove the `DirAccess` scan workaround.

This is the recommended path. The simple fix unblocks the feature today; the engine change is the right long-term architecture but doesn't need to block shipping.
