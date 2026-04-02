/**************************************************************************/
/*  gif_import_handler.h                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#ifndef GIF_IMPORT_HANDLER_H
#define GIF_IMPORT_HANDLER_H

#include "editor/plugins/editor_plugin.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/label.h"
#include "scene/gui/button.h"
#include "core/templates/hash_set.h"
#include "core/io/image.h"

// Listens for new .gif files in the project filesystem and offers
// to convert them to a sprite-sheet PNG.
class GIFImportHandler : public EditorPlugin {
	GDCLASS(GIFImportHandler, EditorPlugin);

	// Dialog shown when a .gif is detected
	ConfirmationDialog *_dialog = nullptr;
	Label *_dialog_label = nullptr;

	// Pending gif path for the active dialog
	String _pending_gif_path;

	// Gif paths we've already prompted about this session (avoid re-prompting)
	HashSet<String> _seen_gifs;

	// Walk EditorFileSystem and collect all .gif res:// paths
	void _collect_gif_files(HashSet<String> &r_paths);

	// Check for newly added .gif files and prompt if found
	void _on_filesystem_changed();

	// Dialog callbacks
	void _on_convert_confirmed();
	void _on_convert_cancelled();

	// Convert a .gif at res:// path to a horizontal sprite-sheet .png
	// Returns the output path or empty string on failure.
	String _convert_gif_to_spritesheet(const String &p_gif_path);

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	virtual String get_plugin_name() const override { return "GIFImportHandler"; }

	GIFImportHandler();
};

#endif // GIF_IMPORT_HANDLER_H
