/**************************************************************************/
/*  gif_import_handler.cpp                                                */
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

#include "gif_import_handler.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "editor/editor_file_system.h"
#include "editor/editor_node.h"
#include "scene/gui/box_container.h"
#include "scene/gui/margin_container.h"

// ============================================================================
// Minimal GIF decoder
// ============================================================================
// Supports GIF87a and GIF89a, RGB/RGBA output, LZW decompression.
// Outputs one RGBA Ref<Image> per frame and places them in a horizontal strip.

namespace GIFDecoder {

struct ColorTable {
	uint8_t r[256], g[256], b[256];
	int size = 0;
};

struct Frame {
	Ref<Image> image; // RGBA, canvas-sized
	int delay_ms = 100;
	int disposal = 0; // Graphic Control disposal method
};

// LZW decompression for GIF.
// p_data: concatenated sub-block payload (blocks already stripped)
// min_code_size: from the Image Descriptor
// expected_pixels: canvas_w * frame_h (used to guard against overrun)
// Returns decoded color indices.
static bool lzw_decompress(const Vector<uint8_t> &p_data, int min_code_size,
		int expected_pixels, Vector<uint8_t> &r_indices) {
	if (min_code_size < 2 || min_code_size > 8) {
		return false;
	}

	const int clear_code = 1 << min_code_size;
	const int eoi_code = clear_code + 1;

	// Code table: (prefix_code, suffix_byte)
	// -1 prefix means root (single-byte) entry.
	struct Entry {
		int prefix = -1;
		uint8_t suffix = 0;
	};
	static const int TABLE_MAX = 4096;
	Entry table[TABLE_MAX];

	auto reset_table = [&](int &next_code, int &code_size) {
		for (int i = 0; i < clear_code; ++i) {
			table[i].prefix = -1;
			table[i].suffix = (uint8_t)i;
		}
		next_code = eoi_code + 1;
		code_size = min_code_size + 1;
	};

	int next_code, code_size;
	reset_table(next_code, code_size);

	// Bit stream reader (LSB first, as GIF specifies)
	int bit_buf = 0, bits_in_buf = 0;
	int data_pos = 0;
	const uint8_t *data = p_data.ptr();
	const int data_size = p_data.size();

	auto read_bits = [&](int n) -> int {
		while (bits_in_buf < n && data_pos < data_size) {
			bit_buf |= (int)(data[data_pos++]) << bits_in_buf;
			bits_in_buf += 8;
		}
		if (bits_in_buf < n) {
			return -1;
		}
		int val = bit_buf & ((1 << n) - 1);
		bit_buf >>= n;
		bits_in_buf -= n;
		return val;
	};

	// Output a code's sequence (reversed walk up the prefix chain)
	// Writes to r_indices in correct order.
	auto output_code = [&](int code) -> uint8_t {
		// Collect sequence by walking up prefix chain
		uint8_t seq[TABLE_MAX];
		int len = 0;
		int c = code;
		while (c >= 0 && len < TABLE_MAX) {
			seq[len++] = table[c].suffix;
			c = table[c].prefix;
		}
		// Reverse and append
		for (int i = len - 1; i >= 0; --i) {
			r_indices.push_back(seq[i]);
		}
		return seq[len - 1]; // first byte of sequence (root suffix)
	};

	r_indices.clear();

	int prev_code = -1;

	while (r_indices.size() < expected_pixels) {
		int code = read_bits(code_size);
		if (code < 0 || code == eoi_code) {
			break;
		}
		if (code == clear_code) {
			reset_table(next_code, code_size);
			prev_code = -1;
			continue;
		}

		bool in_table = (code < next_code);
		uint8_t first_byte;

		if (in_table) {
			first_byte = output_code(code);
		} else if (prev_code >= 0 && code == next_code) {
			// Special: code = next_code, first byte of prev sequence
			// Walk prev_code to get its first byte
			int c = prev_code;
			while (table[c].prefix >= 0) {
				c = table[c].prefix;
			}
			first_byte = table[c].suffix;
			// Output prev + first_byte
			output_code(prev_code);
			r_indices.push_back(first_byte);
		} else {
			// Corrupt stream
			break;
		}

		// Add new entry
		if (prev_code >= 0 && next_code < TABLE_MAX) {
			table[next_code].prefix = prev_code;
			table[next_code].suffix = in_table ? first_byte : first_byte;
			++next_code;
			// Expand code size when we've used all codes at current size
			if (next_code == (1 << code_size) && code_size < 12) {
				++code_size;
			}
		}

		prev_code = code;
	}

	return !r_indices.is_empty();
}

// Read all sub-block bytes (GIF sub-block chain until 0-length block).
static bool read_sub_blocks(const uint8_t *data, int data_len,
		int &pos, Vector<uint8_t> &r_out) {
	while (pos < data_len) {
		int block_size = data[pos++];
		if (block_size == 0) {
			break;
		}
		if (pos + block_size > data_len) {
			return false;
		}
		for (int i = 0; i < block_size; ++i) {
			r_out.push_back(data[pos + i]);
		}
		pos += block_size;
	}
	return true;
}

// Skip sub-blocks without reading.
static void skip_sub_blocks(const uint8_t *data, int data_len, int &pos) {
	while (pos < data_len) {
		int block_size = data[pos++];
		if (block_size == 0) {
			break;
		}
		pos += block_size;
		if (pos > data_len) {
			break;
		}
	}
}

static uint16_t read_u16le(const uint8_t *data, int pos) {
	return (uint16_t)(data[pos]) | ((uint16_t)(data[pos + 1]) << 8);
}

// Main GIF parsing entry point.
// Returns false if the file is not a valid GIF or has no decodable frames.
bool load_frames(const PackedByteArray &p_gif_data, int &r_canvas_w, int &r_canvas_h,
		Vector<Frame> &r_frames) {
	const uint8_t *data = p_gif_data.ptr();
	const int data_len = p_gif_data.size();

	// Header
	if (data_len < 13) {
		return false;
	}
	if (memcmp(data, "GIF87a", 6) != 0 && memcmp(data, "GIF89a", 6) != 0) {
		return false;
	}

	// Logical Screen Descriptor
	r_canvas_w = (int)read_u16le(data, 6);
	r_canvas_h = (int)read_u16le(data, 8);
	uint8_t packed = data[10];
	bool has_gct = (packed >> 7) & 1;
	int gct_size = (packed & 0x07);

	ColorTable gct;
	int pos = 13;

	if (has_gct) {
		gct.size = 2 << gct_size;
		if (pos + gct.size * 3 > data_len) {
			return false;
		}
		for (int i = 0; i < gct.size; ++i) {
			gct.r[i] = data[pos + i * 3 + 0];
			gct.g[i] = data[pos + i * 3 + 1];
			gct.b[i] = data[pos + i * 3 + 2];
		}
		pos += gct.size * 3;
	}

	// State carried between frames
	int delay_ms = 100;
	int disposal = 0;
	bool has_transparency = false;
	int transparent_idx = 0;

	// Canvas (for compositing)
	Vector<uint8_t> canvas;
	canvas.resize(r_canvas_w * r_canvas_h * 4);
	memset(canvas.ptrw(), 0, canvas.size());

	while (pos < data_len) {
		uint8_t block_type = data[pos++];

		if (block_type == 0x3B) {
			// Trailer
			break;
		}

		if (block_type == 0x21) {
			// Extension
			if (pos >= data_len) {
				break;
			}
			uint8_t ext_type = data[pos++];

			if (ext_type == 0xF9) {
				// Graphic Control Extension
				if (pos + 1 > data_len) {
					break;
				}
				int block_size = data[pos++]; // should be 4
				if (pos + block_size > data_len) {
					break;
				}
				uint8_t gc_packed = data[pos];
				has_transparency = (gc_packed & 0x01) != 0;
				disposal = (gc_packed >> 2) & 0x07;
				delay_ms = (int)read_u16le(data, pos + 1) * 10;
				if (delay_ms <= 0) {
					delay_ms = 100;
				}
				transparent_idx = data[pos + 3];
				pos += block_size;
				if (pos < data_len && data[pos] == 0) {
					++pos; // block terminator
				}
			} else {
				// Skip other extensions
				skip_sub_blocks(data, data_len, pos);
			}
			continue;
		}

		if (block_type == 0x2C) {
			// Image Descriptor
			if (pos + 9 > data_len) {
				break;
			}
			int frame_left = (int)read_u16le(data, pos);
			int frame_top = (int)read_u16le(data, pos + 2);
			int frame_w = (int)read_u16le(data, pos + 4);
			int frame_h = (int)read_u16le(data, pos + 6);
			uint8_t img_packed = data[pos + 8];
			pos += 9;

			bool has_lct = (img_packed >> 7) & 1;
			bool interlaced = (img_packed >> 6) & 1;
			int lct_size_bits = img_packed & 0x07;

			ColorTable lct;
			const ColorTable &active_ct = has_lct ? lct : gct;

			if (has_lct) {
				lct.size = 2 << lct_size_bits;
				if (pos + lct.size * 3 > data_len) {
					break;
				}
				for (int i = 0; i < lct.size; ++i) {
					lct.r[i] = data[pos + i * 3 + 0];
					lct.g[i] = data[pos + i * 3 + 1];
					lct.b[i] = data[pos + i * 3 + 2];
				}
				pos += lct.size * 3;
			}

			if (pos >= data_len) {
				break;
			}
			int min_code_size = data[pos++];

			// Read sub-blocks
			Vector<uint8_t> lzw_data;
			read_sub_blocks(data, data_len, pos, lzw_data);

			// Decompress
			Vector<uint8_t> indices;
			if (!lzw_decompress(lzw_data, min_code_size, frame_w * frame_h, indices)) {
				// Skip corrupt frame
				delay_ms = 100;
				has_transparency = false;
				disposal = 0;
				continue;
			}

			// Handle interlacing (de-interlace pass order: 0,8; 4,8; 2,4; 1,2)
			Vector<uint8_t> deinterlaced;
			if (interlaced && frame_w > 0 && frame_h > 0) {
				deinterlaced.resize(frame_w * frame_h);
				static const int pass_start[] = { 0, 4, 2, 1 };
				static const int pass_step[] = { 8, 8, 4, 2 };
				int src_row = 0;
				for (int pass = 0; pass < 4; ++pass) {
					for (int row = pass_start[pass]; row < frame_h; row += pass_step[pass]) {
						if (src_row * frame_w + frame_w > indices.size()) {
							break;
						}
						memcpy(deinterlaced.ptrw() + row * frame_w,
								indices.ptr() + src_row * frame_w,
								frame_w);
						++src_row;
					}
				}
				indices = deinterlaced;
			}

			// Apply disposal method to canvas before drawing this frame
			Vector<uint8_t> prev_canvas;
			if (disposal == 3) {
				// Restore to previous — save current canvas
				prev_canvas = canvas;
			} else if (disposal == 2) {
				// Restore to background — clear the frame region
				for (int row = 0; row < frame_h; ++row) {
					int cy = frame_top + row;
					if (cy < 0 || cy >= r_canvas_h) {
						continue;
					}
					for (int col = 0; col < frame_w; ++col) {
						int cx = frame_left + col;
						if (cx < 0 || cx >= r_canvas_w) {
							continue;
						}
						int ci = (cy * r_canvas_w + cx) * 4;
						canvas.write[ci + 0] = 0;
						canvas.write[ci + 1] = 0;
						canvas.write[ci + 2] = 0;
						canvas.write[ci + 3] = 0;
					}
				}
			}

			// Blit frame onto canvas
			for (int row = 0; row < frame_h; ++row) {
				int cy = frame_top + row;
				if (cy < 0 || cy >= r_canvas_h) {
					continue;
				}
				for (int col = 0; col < frame_w; ++col) {
					int cx = frame_left + col;
					if (cx < 0 || cx >= r_canvas_w) {
						continue;
					}
					int src_idx = row * frame_w + col;
					if (src_idx >= indices.size()) {
						continue;
					}
					uint8_t cidx = indices[src_idx];

					if (has_transparency && cidx == transparent_idx) {
						continue; // transparent pixel — keep canvas as-is
					}

					int ci = (cy * r_canvas_w + cx) * 4;
					canvas.write[ci + 0] = active_ct.r[cidx];
					canvas.write[ci + 1] = active_ct.g[cidx];
					canvas.write[ci + 2] = active_ct.b[cidx];
					canvas.write[ci + 3] = 255;
				}
			}

			// Snapshot canvas as this frame's image
			Ref<Image> img = Image::create_from_data(r_canvas_w, r_canvas_h, false,
					Image::FORMAT_RGBA8, canvas);

			Frame f;
			f.image = img;
			f.delay_ms = delay_ms;
			f.disposal = disposal;
			r_frames.push_back(f);

			// Apply disposal for next frame
			if (disposal == 3 && !prev_canvas.is_empty()) {
				canvas = prev_canvas;
			}

			// Reset per-frame state
			delay_ms = 100;
			has_transparency = false;
			disposal = 0;
			continue;
		}

		// Unknown block type — stop
		break;
	}

	return !r_frames.is_empty();
}

} // namespace GIFDecoder

// ============================================================================
// GIFImportHandler
// ============================================================================

void GIFImportHandler::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_on_filesystem_changed"), &GIFImportHandler::_on_filesystem_changed);
	ClassDB::bind_method(D_METHOD("_on_convert_confirmed"), &GIFImportHandler::_on_convert_confirmed);
	ClassDB::bind_method(D_METHOD("_on_convert_cancelled"), &GIFImportHandler::_on_convert_cancelled);
}

void GIFImportHandler::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			EditorFileSystem *efs = EditorFileSystem::get_singleton();
			if (efs && !efs->is_connected("filesystem_changed", callable_mp(this, &GIFImportHandler::_on_filesystem_changed))) {
				efs->connect("filesystem_changed", callable_mp(this, &GIFImportHandler::_on_filesystem_changed));
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			EditorFileSystem *efs = EditorFileSystem::get_singleton();
			if (efs && efs->is_connected("filesystem_changed", callable_mp(this, &GIFImportHandler::_on_filesystem_changed))) {
				efs->disconnect("filesystem_changed", callable_mp(this, &GIFImportHandler::_on_filesystem_changed));
			}
		} break;
	}
}

void GIFImportHandler::_collect_gif_files(HashSet<String> &r_paths) {
	EditorFileSystem *efs = EditorFileSystem::get_singleton();
	if (!efs) {
		return;
	}
	EditorFileSystemDirectory *root = efs->get_filesystem();
	if (!root) {
		return;
	}

	// Recursive walk via a stack
	Vector<EditorFileSystemDirectory *> stack;
	stack.push_back(root);

	while (!stack.is_empty()) {
		EditorFileSystemDirectory *dir = stack[stack.size() - 1];
		stack.resize(stack.size() - 1);

		for (int i = 0; i < dir->get_file_count(); ++i) {
			String name = dir->get_file(i);
			if (name.get_extension().to_lower() == "gif") {
				r_paths.insert(dir->get_file_path(i));
			}
		}
		for (int i = 0; i < dir->get_subdir_count(); ++i) {
			stack.push_back(dir->get_subdir(i));
		}
	}
}

void GIFImportHandler::_on_filesystem_changed() {
	// Find any .gif files we haven't seen yet
	HashSet<String> current_gifs;
	_collect_gif_files(current_gifs);

	String first_new;
	for (const String &path : current_gifs) {
		if (!_seen_gifs.has(path)) {
			_seen_gifs.insert(path);
			if (first_new.is_empty()) {
				first_new = path;
			}
		}
	}

	if (!first_new.is_empty()) {
		_pending_gif_path = first_new;
		if (_dialog_label) {
			_dialog_label->set_text(
					TTR("GIFs are not supported. Would you like to convert your file to a sprite sheet?\n\n") +
					first_new);
		}
		if (_dialog) {
			_dialog->popup_centered();
		}
	}
}

void GIFImportHandler::_on_convert_confirmed() {
	if (_pending_gif_path.is_empty()) {
		return;
	}
	String out = _convert_gif_to_spritesheet(_pending_gif_path);
	if (!out.is_empty()) {
		print_line(vformat("GIF Import: Converted '%s' → '%s'", _pending_gif_path, out));
		// Rescan so the new PNG appears in the filesystem
		EditorFileSystem::get_singleton()->scan_changes();
	} else {
		ERR_PRINT(vformat("GIF Import: Conversion failed for '%s'", _pending_gif_path));
	}
	_pending_gif_path = "";
}

void GIFImportHandler::_on_convert_cancelled() {
	_pending_gif_path = "";
}

String GIFImportHandler::_convert_gif_to_spritesheet(const String &p_gif_path) {
	// Load raw GIF bytes
	String abs_path = ProjectSettings::get_singleton()->globalize_path(p_gif_path);
	Ref<FileAccess> fa = FileAccess::open(abs_path, FileAccess::READ);
	if (!fa.is_valid()) {
		ERR_PRINT(vformat("GIF Import: Cannot open '%s'", abs_path));
		return "";
	}
	PackedByteArray gif_data = fa->get_buffer(fa->get_length());
	fa.unref();

	int canvas_w = 0, canvas_h = 0;
	Vector<GIFDecoder::Frame> frames;

	if (!GIFDecoder::load_frames(gif_data, canvas_w, canvas_h, frames) || frames.is_empty()) {
		ERR_PRINT(vformat("GIF Import: Failed to decode frames from '%s'", abs_path));
		return "";
	}

	int num_frames = frames.size();
	// Build a horizontal sprite sheet: total width = canvas_w * num_frames
	int sheet_w = canvas_w * num_frames;
	int sheet_h = canvas_h;

	PackedByteArray sheet_data;
	sheet_data.resize(sheet_w * sheet_h * 4);
	memset(sheet_data.ptrw(), 0, sheet_data.size());

	for (int fi = 0; fi < num_frames; ++fi) {
		const Ref<Image> &frame_img = frames[fi].image;
		if (!frame_img.is_valid()) {
			continue;
		}
		// Ensure RGBA8
		Ref<Image> rgba = frame_img->duplicate();
		if (rgba->get_format() != Image::FORMAT_RGBA8) {
			rgba->convert(Image::FORMAT_RGBA8);
		}
		PackedByteArray frame_data = rgba->get_data();

		int ox = fi * canvas_w; // horizontal offset in sprite sheet
		for (int row = 0; row < canvas_h; ++row) {
			int src_off = row * canvas_w * 4;
			int dst_off = (row * sheet_w + ox) * 4;
			memcpy(sheet_data.ptrw() + dst_off, frame_data.ptr() + src_off, canvas_w * 4);
		}
	}

	Ref<Image> sheet = Image::create_from_data(sheet_w, sheet_h, false, Image::FORMAT_RGBA8, sheet_data);
	if (!sheet.is_valid()) {
		ERR_PRINT("GIF Import: Failed to create sprite sheet image");
		return "";
	}

	// Save as .png next to the .gif with _sheet suffix
	String base = p_gif_path.get_basename();
	String out_res_path = base + "_sheet.png";
	String out_abs = ProjectSettings::get_singleton()->globalize_path(out_res_path);

	Error err = sheet->save_png(out_abs);
	if (err != OK) {
		ERR_PRINT(vformat("GIF Import: Failed to save sprite sheet to '%s'", out_abs));
		return "";
	}

	return out_res_path;
}

GIFImportHandler::GIFImportHandler() {
	// Build the dialog
	_dialog = memnew(ConfirmationDialog);
	_dialog->set_title(TTR("GIF Not Supported"));
	_dialog->get_ok_button()->set_text(TTR("Convert GIF"));
	_dialog->add_cancel_button(TTR("Cancel"));
	_dialog->connect("confirmed", callable_mp(this, &GIFImportHandler::_on_convert_confirmed));
	_dialog->connect("canceled", callable_mp(this, &GIFImportHandler::_on_convert_cancelled));

	MarginContainer *margin = memnew(MarginContainer);
	margin->add_theme_constant_override("margin_left", 8);
	margin->add_theme_constant_override("margin_right", 8);
	margin->add_theme_constant_override("margin_top", 4);
	margin->add_theme_constant_override("margin_bottom", 4);
	_dialog->add_child(margin);

	_dialog_label = memnew(Label);
	_dialog_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	_dialog_label->set_custom_minimum_size(Size2(400, 0));
	margin->add_child(_dialog_label);

	EditorNode::get_singleton()->get_gui_base()->add_child(_dialog);
}
