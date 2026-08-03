/**************************************************************************/
/*  ai_status_indicator.cpp                                               */
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

#include "ai_status_indicator.h"

#include "../ai.h"
#include "core/templates/hash_map.h"
#include "../agentic_orchestrator.h"
#include "../harness/codex_harness_driver.h"
#include "core/config/engine.h"
#include "core/core_bind.h"
#include "core/input/input_event.h"
#include "core/io/image.h"
#include "core/io/json.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/string/ustring.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/script_editor_debugger.h"
#include "editor/editor_node.h"
#include "editor/editor_settings.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/gui/editor_run_bar.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/rich_text_label.h"
#include "scene/resources/image_texture.h"
#include "scene/gui/margin_container.h"
#include "scene/resources/style_box.h"
#include "scene/resources/style_box_flat.h"
#include "scene/resources/style_box_line.h"
#include "scene/resources/font.h"
#include "scene/scene_string_names.h"
#include "servers/display_server.h"

// Forward declarations for error bubble helpers (defined later, used in _rebuild_message_list)
enum ErrorBubbleFormat {
	ERROR_BUBBLE_RUNTIME, // [E]/[W] message (file:line) xN
	ERROR_BUBBLE_PARSE,   // Line N: message
};
static Control *_create_error_bubble_impl(
		const String &p_summary, const Array &p_errors,
		const Color &p_bg_color, const Color &p_border_color, const Color &p_text_color,
		ErrorBubbleFormat p_format);

// Fallback char budget when model context window is unknown
static const int DEFAULT_MAX_CONTEXT_CHARS = 120000;

// Maximum image size (longest side) before downscaling for API submission.
// Keeps base64 payload small; OpenAI "low" detail processes at 512px for 85 tokens.
static const int IMAGE_MAX_SIDE_PX = 512;

// Resize p_image in-place to fit within IMAGE_MAX_SIDE_PX, convert to RGBA8,
// save as PNG, and return a base64-encoded string. Returns empty string on failure.
static String _encode_image_for_api(Ref<Image> p_image) {
	if (p_image.is_null() || p_image->is_empty()) {
		return "";
	}
	// Ensure a format that PNG encoder accepts
	Image::Format fmt = p_image->get_format();
	if (fmt != Image::FORMAT_RGBA8 && fmt != Image::FORMAT_RGB8) {
		p_image->convert(Image::FORMAT_RGBA8);
	}
	int w = p_image->get_width();
	int h = p_image->get_height();
	int max_side = MAX(w, h);
	if (max_side > IMAGE_MAX_SIDE_PX) {
		float scale = (float)IMAGE_MAX_SIDE_PX / (float)max_side;
		int new_w = MAX(1, (int)(w * scale));
		int new_h = MAX(1, (int)(h * scale));
		p_image->resize(new_w, new_h, Image::INTERPOLATE_LANCZOS);
	}
	Vector<uint8_t> png_bytes = p_image->save_png_to_buffer();
	if (png_bytes.is_empty()) {
		return "";
	}
	return CoreBind::Marshalls::get_singleton()->raw_to_base64(png_bytes);
}

// ============================================================================
// Aristotle Design Tokens (shared with global editor theme)
// ============================================================================
#include "editor/themes/aristotle_tokens.h"

// Local aliases so the 151 existing AIColors:: references keep compiling.
namespace AIColors {
	// Backgrounds
	static const Color BG_0 = Aristotle::BG_0;
	static const Color BG_1 = Aristotle::BG_1;
	static const Color BG_2 = Aristotle::BG_2;
	static const Color BG_3 = Aristotle::BG_3;

	// Borders
	static const Color BORDER = Aristotle::BORDER;
	static const Color BORDER_LIGHT = Aristotle::BORDER_LIGHT;

	// Text
	static const Color TEXT_PRIMARY = Aristotle::TEXT_PRIMARY;
	static const Color TEXT_SECONDARY = Aristotle::TEXT_SECONDARY;
	static const Color TEXT_MUTED = Aristotle::TEXT_MUTED;
	static const Color TEXT_DISABLED = Aristotle::TEXT_DISABLED;

	// Accent - Blue
	static const Color ACCENT_BLUE = Aristotle::ACCENT;
	static const Color ACCENT_BLUE_HOVER = Aristotle::ACCENT_HOVER;
	static const Color ACCENT_BLUE_PRESSED = Aristotle::ACCENT_PRESSED;
	static const Color ACCENT_BLUE_MUTED = Aristotle::ACCENT_MUTED;

	// Assistant bubble (AI panel specific, not in global tokens)
	static const Color ASSISTANT_BG = Color(0.13, 0.14, 0.17, 1.0); // #21242B

	// Status
	static const Color SUCCESS = Aristotle::STATUS_SUCCESS;
	static const Color ERROR = Aristotle::STATUS_ERROR;
	static const Color WARNING = Aristotle::STATUS_WARNING;

	// Spacing
	static const int CORNER_RADIUS_SM = Aristotle::RADIUS_SM;
	static const int CORNER_RADIUS_MD = Aristotle::RADIUS_MD;
	static const int CORNER_RADIUS_LG = Aristotle::RADIUS_LG;
	static const int PADDING_XS = Aristotle::PAD_XS;
	static const int PADDING_SM = Aristotle::PAD_SM;
	static const int PADDING_MD = Aristotle::PAD_MD;
	static const int PADDING_LG = Aristotle::PAD_LG;
}

// ============================================================================
// ToolCollapsibleEntry - Collapsible widget for tool results
// ============================================================================

void ToolCollapsibleEntry::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_on_toggle_pressed"), &ToolCollapsibleEntry::_on_toggle_pressed);
	ClassDB::bind_method(D_METHOD("_on_header_gui_input", "event"), &ToolCollapsibleEntry::_on_header_gui_input);
}

void ToolCollapsibleEntry::_on_toggle_pressed() {
	set_collapsed(!is_collapsed);
}

void ToolCollapsibleEntry::_on_header_gui_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MouseButton::LEFT) {
		set_collapsed(!is_collapsed);
	}
}

void ToolCollapsibleEntry::_update_toggle_icon() {
	if (toggle_button) {
		// Use unicode arrows: ▶ (collapsed) and ▼ (expanded)
		toggle_button->set_text(is_collapsed ? String::utf8("▶") : String::utf8("▼"));
	}
}

void ToolCollapsibleEntry::set_collapsed(bool p_collapsed) {
	if (is_collapsed == p_collapsed) {
		return;
	}

	is_collapsed = p_collapsed;
	_update_toggle_icon();
	_animate_to_collapsed(is_collapsed);
}

void ToolCollapsibleEntry::_animate_to_collapsed(bool p_collapsed) {
	if (_collapse_tween.is_valid() && _collapse_tween->is_valid()) {
		Ref<Tween> prev = _collapse_tween;
		prev->kill();
		_collapse_tween.unref();
	}

	const float duration = 0.15f;
	const bool has_stack = body_stack && body_stack->get_image_count() > 0;

	float body_target_h = 0.0f;
	float stack_target_h = 0.0f;

	if (!p_collapsed) {
		// Expanding — show targets first so layout populates their natural sizes.
		if (body_container) {
			body_container->show();
			body_container->set_custom_minimum_size(Size2(0, 0));
			body_target_h = body_container->get_combined_minimum_size().y;
			body_container->set_clip_contents(true);
			body_container->set_modulate(Color(1, 1, 1, 0));
			body_container->set_custom_minimum_size(Size2(0, 0));
		}
		if (has_stack) {
			body_stack->show();
			stack_target_h = 200.0f * EDSCALE;
			body_stack->set_clip_contents(true);
			body_stack->set_modulate(Color(1, 1, 1, 0));
			body_stack->set_custom_minimum_size(Size2(0, 0));
		}
	} else {
		// Collapsing — start from currently rendered state.
		if (body_container) {
			body_container->set_clip_contents(true);
		}
		if (has_stack) {
			body_stack->set_clip_contents(true);
		}
	}

	_collapse_tween = create_tween();
	_collapse_tween->set_parallel(true);
	_collapse_tween->set_trans(Tween::TRANS_CUBIC);
	_collapse_tween->set_ease(Tween::EASE_OUT);
	const float a_target = p_collapsed ? 0.0f : 1.0f;
	if (body_container) {
		_collapse_tween->tween_property(body_container, NodePath("modulate:a"), a_target, duration);
		_collapse_tween->tween_property(body_container, NodePath("custom_minimum_size:y"), p_collapsed ? 0.0f : body_target_h, duration);
	}
	if (has_stack) {
		_collapse_tween->tween_property(body_stack, NodePath("modulate:a"), a_target, duration);
		_collapse_tween->tween_property(body_stack, NodePath("custom_minimum_size:y"), p_collapsed ? 0.0f : stack_target_h, duration);
	}
	_collapse_tween->chain()->tween_callback(callable_mp(this, &ToolCollapsibleEntry::_on_collapse_finished));
}

void ToolCollapsibleEntry::_on_collapse_finished() {
	const bool has_stack = body_stack && body_stack->get_image_count() > 0;

	if (body_container) {
		body_container->set_clip_contents(false);
		body_container->set_modulate(Color(1, 1, 1, 1));
		body_container->set_custom_minimum_size(Size2(0, 0));
		if (is_collapsed) {
			body_container->hide();
		}
	}
	if (has_stack) {
		body_stack->set_clip_contents(false);
		body_stack->set_modulate(Color(1, 1, 1, 1));
		// Restore the default thumbnail height so layout sizes the stack like
		// it did before the animation started.
		body_stack->set_custom_minimum_size(Size2(0, 200.0f * EDSCALE));
		if (is_collapsed) {
			body_stack->hide();
		}
	}
	_collapse_tween.unref();
}

bool ToolCollapsibleEntry::get_collapsed() const {
	return is_collapsed;
}

void ToolCollapsibleEntry::set_header(const String &p_text, const String &p_status) {
	if (header_label) {
		header_label->set_text(p_text);
	}
	if (status_label) {
		status_label->set_text(p_status);
		status_label->set_visible(!p_status.is_empty());
	}
}

void ToolCollapsibleEntry::set_body(const String &p_text) {
	if (body_text) {
		body_text->set_text(p_text);
		// Auto-size height based on content (approximate lines * line height)
		int line_count = p_text.get_slice_count("\n");
		int min_height = MAX(60, (line_count + 1) * 18) * EDSCALE;
		body_text->set_custom_minimum_size(Size2(0, min_height));
	}
}

void ToolCollapsibleEntry::set_token_label_visible(bool p_visible) {
	if (token_label) {
		token_label->set_visible(p_visible && !token_label->get_text().is_empty());
	}
}

void ToolCollapsibleEntry::set_pending_glyph(const String &p_glyph) {
	if (!is_pending || !status_label) {
		return;
	}
	status_label->set_text(p_glyph);
	status_label->set_visible(true);
}

void ToolCollapsibleEntry::update_from_tool_result(const Dictionary &p_tool_result) {
	String action_type = p_tool_result.get("type", "unknown");
	String status = p_tool_result.get("status", "unknown");

	// Build header text
	String header_text = vformat("[Tool] %s", action_type);

	// Pending render: show only the tool name + args, muted border, no status glyph.
	// AIStatusPanel drives an animated spinner into the status label via set_pending_glyph.
	if (status == "pending") {
		is_pending = true;
		set_header(header_text, String::utf8("⠋"));
		if (status_label) {
			status_label->add_theme_color_override("font_color", AIColors::TEXT_MUTED);
		}
		if (panel_style.is_valid()) {
			panel_style->set_border_color(AIColors::ACCENT_BLUE_MUTED);
		}
		String body_content;
		if (p_tool_result.has("args") && p_tool_result["args"].get_type() == Variant::DICTIONARY) {
			Dictionary args = p_tool_result["args"];
			if (!args.is_empty()) {
				body_content = vformat("Args: %s", JSON::stringify(args, "  ", false));
			}
		}
		set_body(body_content);
		return;
	}

	is_pending = false;

	// Build status indicator
	String status_text;
	if (status == "success") {
		status_text = String::utf8("✓");
	} else if (status == "error") {
		status_text = String::utf8("✗");
	}

	set_header(header_text, status_text);

	// Build body text with full details
	String body_content;

	// Args first (input to the tool)
	if (p_tool_result.has("args") && p_tool_result["args"].get_type() == Variant::DICTIONARY) {
		Dictionary args = p_tool_result["args"];
		if (!args.is_empty()) {
			body_content += vformat("Args: %s\n", JSON::stringify(args, "  ", false));
		}
	}

	if (status == "success") {
		body_content += "Status: Success";
		if (p_tool_result.has("result")) {
			Dictionary result = p_tool_result["result"];
			if (!result.is_empty()) {
				// Collect every base64 image this result carries, in display
				// order (chronological for run_and_screenshot, source order for
				// preview_asset). Three shapes are recognised:
				//   - result.screenshot_b64           (single-shot tools)
				//   - result.screenshots[].screenshot_b64  (multi-shot run_and_screenshot)
				//   - result._images[]                (preview_asset)
				PackedStringArray collected_b64s;
				Dictionary display_result = result.duplicate();
				bool stripped_for_display = false;

				if (result.has("screenshot_b64")) {
					collected_b64s.push_back(result["screenshot_b64"]);
					display_result.erase("screenshot_b64");
					display_result["screenshot"] = "<image>";
					stripped_for_display = true;
				}
				if (result.has("screenshots") && result["screenshots"].get_type() == Variant::ARRAY) {
					Array shots = result["screenshots"];
					Array shots_for_display;
					for (int si = 0; si < shots.size(); si++) {
						Dictionary entry = shots[si];
						Dictionary stripped = entry.duplicate();
						if (entry.has("screenshot_b64")) {
							collected_b64s.push_back(entry["screenshot_b64"]);
							stripped.erase("screenshot_b64");
							stripped["screenshot"] = vformat("<image %d>", collected_b64s.size());
						}
						shots_for_display.push_back(stripped);
					}
					display_result["screenshots"] = shots_for_display;
					stripped_for_display = true;
				}
				if (result.has("_images") && result["_images"].get_type() == Variant::ARRAY) {
					Array imgs = result["_images"];
					for (int ii = 0; ii < imgs.size(); ii++) {
						String s = imgs[ii];
						if (!s.is_empty()) {
							collected_b64s.push_back(s);
						}
					}
					display_result.erase("_images");
					display_result["_images_count"] = imgs.size();
					stripped_for_display = true;
				}

				if (stripped_for_display) {
					body_content += vformat("\nResult:\n%s", JSON::stringify(display_result, "  ", false));
				} else {
					body_content += vformat("\nResult:\n%s", JSON::stringify(result, "  ", false));
				}

				if (!collected_b64s.is_empty() && body_stack) {
					Vector<Ref<Texture2D>> texs;
					PackedStringArray decoded_b64s;
					for (int i = 0; i < collected_b64s.size(); i++) {
						const String &b64 = collected_b64s[i];
						PackedByteArray png_bytes = CoreBind::Marshalls::get_singleton()->base64_to_raw(b64);
						if (png_bytes.is_empty()) {
							continue;
						}
						Ref<Image> img;
						img.instantiate();
						if (img->load_png_from_buffer(png_bytes) != OK) {
							continue;
						}
						texs.push_back(ImageTexture::create_from_image(img));
						decoded_b64s.push_back(b64);
					}
					body_stack->set_images(texs);
					screenshot_b64s = decoded_b64s;
				}
			}
		}
	} else if (status == "error") {
		body_content += "Status: Error";
		if (p_tool_result.has("error")) {
			Dictionary error = p_tool_result["error"];
			String error_msg = error.get("message", "Unknown error");
			String error_code = error.get("code", "");
			if (!error_code.is_empty()) {
				body_content += vformat("\nCode: %s", error_code);
			}
			body_content += vformat("\nMessage: %s", error_msg);
			// Show what the model was actually trying to say
			if (error.has("details") && error["details"].get_type() == Variant::DICTIONARY) {
				Dictionary details = error["details"];
				String raw = details.get("raw_response_preview", "");
				if (!raw.is_empty()) {
					body_content += vformat("\n\nModel attempted:\n%s", raw);
				}
			}
		}
	} else {
		body_content = vformat("Status: %s", status);
	}

	set_body(body_content);

	// Token count label
	if (p_tool_result.has("tokens") && token_label) {
		int tokens = (int)p_tool_result["tokens"];
		if (tokens > 0) {
			String token_text;
			if (tokens >= 10000) {
				token_text = vformat("%dk", tokens / 1000);
			} else {
				token_text = itos(tokens);
			}
			token_label->set_text(vformat("Token estimate: %s", token_text));
			// Visibility is controlled by the global toggle — don't force show here.
		}
	}

	// Update status label color and panel border tint.
	if (status == "success") {
		if (status_label) {
			status_label->add_theme_color_override("font_color", AIColors::SUCCESS);
		}
		if (panel_style.is_valid()) {
			panel_style->set_border_color(Color(AIColors::SUCCESS.r, AIColors::SUCCESS.g, AIColors::SUCCESS.b, 0.5f));
		}
	} else if (status == "error") {
		if (status_label) {
			status_label->add_theme_color_override("font_color", AIColors::ERROR);
		}
		if (panel_style.is_valid()) {
			panel_style->set_border_color(Color(AIColors::ERROR.r, AIColors::ERROR.g, AIColors::ERROR.b, 0.6f));
		}
	}
}

ToolCollapsibleEntry::ToolCollapsibleEntry() {
	set_h_size_flags(SIZE_EXPAND_FILL);

	// Create main panel with background
	PanelContainer *main_panel = memnew(PanelContainer);
	main_panel->set_h_size_flags(SIZE_EXPAND_FILL);
	add_child(main_panel);

	// Style the panel with subtle border — stored as member so we can tint it on result.
	panel_style.instantiate();
	panel_style->set_bg_color(AIColors::BG_1);
	panel_style->set_border_width_all(1);
	panel_style->set_border_color(AIColors::BORDER);
	panel_style->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
	panel_style->set_corner_radius_all(AIColors::CORNER_RADIUS_MD * EDSCALE);
	main_panel->add_theme_style_override("panel", panel_style);

	// Inner VBox for header and body
	VBoxContainer *inner_vbox = memnew(VBoxContainer);
	inner_vbox->set_h_size_flags(SIZE_EXPAND_FILL);
	inner_vbox->add_theme_constant_override("separation", AIColors::PADDING_XS * EDSCALE);
	main_panel->add_child(inner_vbox);

	// Header row
	header_container = memnew(HBoxContainer);
	header_container->set_h_size_flags(SIZE_EXPAND_FILL);
	header_container->add_theme_constant_override("separation", AIColors::PADDING_SM * EDSCALE);
	header_container->set_mouse_filter(MOUSE_FILTER_STOP);
	header_container->connect("gui_input", callable_mp(this, &ToolCollapsibleEntry::_on_header_gui_input));
	inner_vbox->add_child(header_container);

	// Toggle button (chevron) - styled flat button
	toggle_button = memnew(Button);
	toggle_button->set_flat(true);
	toggle_button->set_text(String::utf8("▶")); // Right-pointing triangle (collapsed)
	toggle_button->set_custom_minimum_size(Size2(20 * EDSCALE, 0));
	toggle_button->add_theme_color_override("font_color", AIColors::TEXT_MUTED);
	toggle_button->add_theme_color_override("font_hover_color", AIColors::TEXT_PRIMARY);
	toggle_button->add_theme_color_override("font_pressed_color", AIColors::TEXT_PRIMARY);
	toggle_button->connect(SceneStringNames::get_singleton()->pressed, callable_mp(this, &ToolCollapsibleEntry::_on_toggle_pressed));
	header_container->add_child(toggle_button);

	// Header label
	header_label = memnew(Label);
	header_label->set_h_size_flags(SIZE_EXPAND_FILL);
	header_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	header_label->add_theme_color_override("font_color", AIColors::TEXT_PRIMARY);
	header_container->add_child(header_label);

	// Token count label (hidden until tokens arrive; visibility toggled by user)
	token_label = memnew(Label);
	token_label->set_visible(false);
	token_label->add_theme_color_override("font_color", AIColors::TEXT_MUTED);
	header_container->add_child(token_label);

	// Status label (emoji indicator)
	status_label = memnew(Label);
	status_label->set_visible(false);
	header_container->add_child(status_label);

	// Body container (hidden by default)
	body_container = memnew(PanelContainer);
	body_container->set_h_size_flags(SIZE_EXPAND_FILL);
	body_container->set_visible(false); // Collapsed by default
	inner_vbox->add_child(body_container);

	// Style the body container
	Ref<StyleBoxFlat> body_style;
	body_style.instantiate();
	body_style->set_bg_color(AIColors::BG_0);
	body_style->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
	body_style->set_corner_radius_all(AIColors::CORNER_RADIUS_SM * EDSCALE);
	body_container->add_theme_style_override("panel", body_style);

	// Body text (read-only TextEdit for proper indentation handling)
	body_text = memnew(TextEdit);
	body_text->set_h_size_flags(SIZE_EXPAND_FILL);
	body_text->set_editable(false);
	body_text->set_context_menu_enabled(false);
	body_text->set_shortcut_keys_enabled(true); // Allow Ctrl+C to copy selected text
	body_text->set_selecting_enabled(true);
	body_text->set_line_wrapping_mode(TextEdit::LINE_WRAPPING_BOUNDARY);
	body_text->set_custom_minimum_size(Size2(0, 60 * EDSCALE));
	body_text->add_theme_color_override("font_color", AIColors::TEXT_SECONDARY);
	body_text->add_theme_color_override("background_color", AIColors::BG_0);
	// Use a flat style for seamless look
	Ref<StyleBoxFlat> text_style;
	text_style.instantiate();
	text_style->set_bg_color(AIColors::BG_0);
	text_style->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
	body_text->add_theme_style_override("normal", text_style);
	body_text->add_theme_style_override("read_only", text_style);
	body_container->add_child(body_text);

	// Screenshot displayed below the text body, toggled by the same collapse button
	body_stack = memnew(AIImageStack);
	body_stack->set_h_size_flags(SIZE_EXPAND_FILL);
	body_stack->hide();
	inner_vbox->add_child(body_stack);
}

// ============================================================================
// ParseErrorPill - Shows parse errors from script actions above the input box
// ============================================================================

void ParseErrorPill::_bind_methods() {
}

void ParseErrorPill::_on_header_clicked(const Ref<InputEvent> &p_event) {
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->get_button_index() == MouseButton::LEFT && mb->is_pressed()) {
		_set_expanded(!_expanded);
	}
}

void ParseErrorPill::_set_expanded(bool p_expanded) {
	_expanded = p_expanded;
	if (_error_list) {
		_error_list->set_visible(_expanded);
	}
	_rebuild_ui();
}

void ParseErrorPill::set_errors(const String &p_file_path, const Array &p_errors) {
	_file_path = p_file_path;
	_errors = p_errors;

	set_visible(!_errors.is_empty());
	if (_errors.is_empty() && _expanded) {
		_set_expanded(false);
	}
	_rebuild_ui();
}

void ParseErrorPill::clear_for_file(const String &p_file_path) {
	if (_file_path == p_file_path) {
		clear_all();
	}
}

void ParseErrorPill::clear_all() {
	_file_path = "";
	_errors.clear();
	set_visible(false);
	_set_expanded(false);
	_rebuild_ui();
}

String ParseErrorPill::get_context_summary() const {
	if (_errors.is_empty()) {
		return String();
	}
	return vformat("%d parse error%s in %s",
			_errors.size(), _errors.size() == 1 ? "" : "s", _file_path.get_file());
}

void ParseErrorPill::_rebuild_ui() {
	if (!_main_label || !_error_list) {
		return;
	}

	// Header label
	if (_errors.is_empty()) {
		_main_label->set_text("");
	} else {
		String filename = _file_path.get_file();
		String arrow = _expanded ? String::utf8("\u25BC ") : String::utf8("\u25B6 "); // ▼ or ▶
		_main_label->set_text(vformat("%s%d parse error%s in %s",
				arrow, _errors.size(), _errors.size() == 1 ? "" : "s", filename));
	}

	// Rebuild error list
	while (_error_list->get_child_count() > 0) {
		Node *child = _error_list->get_child(0);
		_error_list->remove_child(child);
		child->queue_free();
	}

	for (int i = 0; i < _errors.size(); i++) {
		Dictionary err = _errors[i];
		String message = err.get("message", "Unknown error");
		int line = err.get("line", 0);
		String type = err.get("type", "syntax");

		String entry_text;
		if (line > 0) {
			entry_text = vformat("Line %d: %s", line, message);
		} else {
			entry_text = message;
		}

		Label *entry_label = memnew(Label);
		entry_label->set_text(entry_text);
		entry_label->add_theme_font_size_override("font_size", 10 * EDSCALE);
		entry_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
		entry_label->add_theme_color_override("font_color", Color(1.0f, 0.6f, 0.3f, 0.8f));
		_error_list->add_child(entry_label);
	}
}

ParseErrorPill::ParseErrorPill() {
	set_visible(false);
	add_theme_constant_override("separation", 0);

	// Orange-tinted style for parse errors (distinct from red runtime errors)
	_style.instantiate();
	_style->set_bg_color(Color(0.0f, 0.0f, 0.0f, 0.0f));
	_style->set_border_width_all(1);
	_style->set_border_color(Color(0.8f, 0.5f, 0.2f, 0.6f));
	_style->set_corner_radius_all(6 * EDSCALE);
	_style->set_content_margin_all(6 * EDSCALE);

	// Main pill panel
	_pill_container = memnew(PanelContainer);
	_pill_container->add_theme_style_override("panel", _style);
	_pill_container->connect("gui_input", callable_mp(this, &ParseErrorPill::_on_header_clicked));
	add_child(_pill_container);

	// Inner VBox for header + expandable list
	VBoxContainer *pill_vbox = memnew(VBoxContainer);
	pill_vbox->add_theme_constant_override("separation", 4 * EDSCALE);
	_pill_container->add_child(pill_vbox);

	// Header row
	_header_row = memnew(HBoxContainer);
	_header_row->set_h_size_flags(SIZE_EXPAND_FILL);
	pill_vbox->add_child(_header_row);

	// Label
	_main_label = memnew(RichTextLabel);
	_main_label->set_use_bbcode(false);
	_main_label->set_fit_content(true);
	_main_label->set_scroll_active(false);
	_main_label->set_h_size_flags(SIZE_EXPAND_FILL);
	_main_label->set_v_size_flags(SIZE_SHRINK_CENTER);
	_main_label->set_mouse_filter(MOUSE_FILTER_IGNORE);
	_main_label->add_theme_color_override("default_color", Color(1.0f, 0.6f, 0.3f, 1.0f));
	_main_label->add_theme_font_size_override("normal_font_size", 11 * EDSCALE);
	_header_row->add_child(_main_label);

	// Expandable error list (hidden by default)
	_error_list = memnew(VBoxContainer);
	_error_list->set_visible(false);
	_error_list->add_theme_constant_override("separation", 2 * EDSCALE);
	pill_vbox->add_child(_error_list);
}

// ============================================================================
// DebugContextPill - Shows game session errors above the input box
// ============================================================================

void DebugContextPill::_bind_methods() {
	ADD_SIGNAL(MethodInfo("enabled_changed", PropertyInfo(Variant::BOOL, "enabled")));
}

void DebugContextPill::_rebuild_label() {
	if (!_main_label || !_toggle_btn) {
		return;
	}

	// Build counts string: "N errors, M warnings" / "N errors" / "M warnings"
	String counts;
	if (_error_count > 0 && _warning_count > 0) {
		counts = vformat("%d error%s, %d warning%s",
				_error_count, _error_count == 1 ? "" : "s",
				_warning_count, _warning_count == 1 ? "" : "s");
	} else if (_error_count > 0) {
		counts = vformat("%d error%s", _error_count, _error_count == 1 ? "" : "s");
	} else {
		counts = vformat("%d warning%s", _warning_count, _warning_count == 1 ? "" : "s");
	}

	// Use fixed-width prefix so the count doesn't shift when toggling
	// "Excluding" is 9 chars, "Including" is 9 chars — same length, no shift
	String prefix = _enabled ? "Including" : "Excluding";
	String arrow = _expanded ? String::utf8("\u25BC ") : String::utf8("\u25B6 "); // ▼ or ▶
	_main_label->set_text(vformat("%s%s  %s", arrow, prefix, counts));

	_toggle_btn->set_text(_enabled ? "exclude" : "include");

	// Swap styles
	_pill_container->add_theme_style_override("panel", _enabled ? _style_enabled : _style_disabled);
	Color text_color = _enabled ? Color(1.0f, 0.5f, 0.45f, 1.0f) : Color(0.6f, 0.6f, 0.6f, 1.0f);
	_main_label->add_theme_color_override("default_color", text_color);
	_toggle_btn->add_theme_color_override("font_color", text_color);
	_toggle_btn->add_theme_color_override("font_hover_color", text_color);
	_toggle_btn->add_theme_color_override("font_pressed_color", text_color);
}

void DebugContextPill::_on_toggle_pressed() {
	_enabled = !_enabled;
	_rebuild_label();
	emit_signal("enabled_changed", _enabled);
}

void DebugContextPill::update_state(bool p_game_running, int p_error_count, int p_warning_count, const Array &p_errors) {
	_game_running = p_game_running;
	_error_count = p_error_count;
	_warning_count = p_warning_count;
	_errors = p_errors;

	bool should_show = (_error_count + _warning_count) > 0;
	set_visible(should_show);
	if (!should_show && _expanded) {
		_set_expanded(false);
	}
	_rebuild_label();
	_rebuild_error_list();
}

void DebugContextPill::_on_header_clicked(const Ref<InputEvent> &p_event) {
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->get_button_index() == MouseButton::LEFT && mb->is_pressed()) {
		_set_expanded(!_expanded);
	}
}

void DebugContextPill::_set_expanded(bool p_expanded) {
	_expanded = p_expanded;
	if (_error_list) {
		_error_list->set_visible(_expanded);
	}
	_rebuild_label();
}

void DebugContextPill::_rebuild_error_list() {
	if (!_error_list) {
		return;
	}

	// Clear existing entries
	while (_error_list->get_child_count() > 0) {
		Node *child = _error_list->get_child(0);
		_error_list->remove_child(child);
		child->queue_free();
	}

	for (int i = 0; i < _errors.size(); i++) {
		Dictionary err = _errors[i];
		String severity = err.get("severity", "error");
		String message = err.get("message", "Unknown error");
		String script = err.get("script", "");
		int line = err.get("line", 0);
		int occurrences = err.get("occurrences", 1);

		String entry_text;
		if (severity == "warning") {
			entry_text = vformat("[W] %s", message);
		} else {
			entry_text = vformat("[E] %s", message);
		}
		if (!script.is_empty() && line > 0) {
			entry_text += vformat("  (%s:%d)", script.get_file(), line);
		}
		if (occurrences > 1) {
			entry_text += vformat("  x%d", occurrences);
		}

		Label *entry_label = memnew(Label);
		entry_label->set_text(entry_text);
		entry_label->add_theme_font_size_override("font_size", 10 * EDSCALE);
		entry_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);

		Color entry_color = (severity == "warning")
				? Color(1.0f, 0.85f, 0.4f, 0.8f)
				: Color(1.0f, 0.5f, 0.45f, 0.8f);
		if (!_enabled) {
			entry_color = Color(0.5f, 0.5f, 0.5f, 0.6f);
		}
		entry_label->add_theme_color_override("font_color", entry_color);
		_error_list->add_child(entry_label);
	}
}

String DebugContextPill::get_context_summary() const {
	if (_error_count == 0 && _warning_count == 0) {
		return String();
	}
	String counts;
	if (_error_count > 0 && _warning_count > 0) {
		counts = vformat("%d error%s, %d warning%s",
				_error_count, _error_count == 1 ? "" : "s",
				_warning_count, _warning_count == 1 ? "" : "s");
	} else if (_error_count > 0) {
		counts = vformat("%d error%s", _error_count, _error_count == 1 ? "" : "s");
	} else {
		counts = vformat("%d warning%s", _warning_count, _warning_count == 1 ? "" : "s");
	}
	return vformat("Including  %s", counts);
}

DebugContextPill::DebugContextPill() {
	set_visible(false);
	add_theme_constant_override("separation", 0);

	// Pre-build enabled/disabled styles
	_style_enabled.instantiate();
	_style_enabled->set_bg_color(Color(0.0f, 0.0f, 0.0f, 0.0f));
	_style_enabled->set_border_width_all(1);
	_style_enabled->set_border_color(Color(0.7f, 0.3f, 0.3f, 0.6f));
	_style_enabled->set_corner_radius_all(6 * EDSCALE);
	_style_enabled->set_content_margin_all(6 * EDSCALE);

	_style_disabled.instantiate();
	_style_disabled->set_bg_color(Color(0.0f, 0.0f, 0.0f, 0.0f));
	_style_disabled->set_border_width_all(1);
	_style_disabled->set_border_color(Color(0.4f, 0.4f, 0.4f, 0.4f));
	_style_disabled->set_corner_radius_all(6 * EDSCALE);
	_style_disabled->set_content_margin_all(6 * EDSCALE);

	// Main pill panel
	_pill_container = memnew(PanelContainer);
	_pill_container->add_theme_style_override("panel", _style_enabled);
	add_child(_pill_container);

	// Inner VBox to hold header + expandable error list inside the PanelContainer
	VBoxContainer *pill_vbox = memnew(VBoxContainer);
	pill_vbox->add_theme_constant_override("separation", 4 * EDSCALE);
	_pill_container->add_child(pill_vbox);

	// Header row
	_header_row = memnew(HBoxContainer);
	_header_row->set_h_size_flags(SIZE_EXPAND_FILL);
	pill_vbox->add_child(_header_row);

	// Label (left side)
	_main_label = memnew(RichTextLabel);
	_main_label->set_use_bbcode(false);
	_main_label->set_fit_content(true);
	_main_label->set_scroll_active(false);
	_main_label->set_h_size_flags(SIZE_EXPAND_FILL);
	_main_label->set_v_size_flags(SIZE_SHRINK_CENTER);
	_main_label->set_mouse_filter(MOUSE_FILTER_IGNORE);
	_main_label->add_theme_color_override("default_color", Color(1.0f, 0.5f, 0.45f, 1.0f));
	_main_label->add_theme_font_size_override("normal_font_size", 11 * EDSCALE);
	_header_row->add_child(_main_label);

	// Toggle button (right side) — no border, just text
	_toggle_btn = memnew(Button);
	_toggle_btn->set_text("exclude");
	_toggle_btn->set_v_size_flags(SIZE_SHRINK_CENTER);
	_toggle_btn->add_theme_color_override("font_color", Color(1.0f, 0.5f, 0.45f, 1.0f));
	_toggle_btn->add_theme_color_override("font_hover_color", Color(1.0f, 0.5f, 0.45f, 1.0f));
	_toggle_btn->add_theme_color_override("font_pressed_color", Color(1.0f, 0.5f, 0.45f, 1.0f));
	_toggle_btn->add_theme_font_size_override("font_size", 11 * EDSCALE);

	// Flat transparent style — no border to avoid double-border with pill container
	Ref<StyleBoxFlat> btn_style;
	btn_style.instantiate();
	btn_style->set_bg_color(Color(0.0f, 0.0f, 0.0f, 0.0f));
	btn_style->set_border_width_all(0);
	btn_style->set_content_margin(SIDE_LEFT, 8 * EDSCALE);
	btn_style->set_content_margin(SIDE_RIGHT, 8 * EDSCALE);
	btn_style->set_content_margin(SIDE_TOP, 2 * EDSCALE);
	btn_style->set_content_margin(SIDE_BOTTOM, 2 * EDSCALE);
	_toggle_btn->add_theme_style_override("normal", btn_style);
	_toggle_btn->add_theme_style_override("hover", btn_style);
	_toggle_btn->add_theme_style_override("pressed", btn_style);
	_toggle_btn->connect(SceneStringNames::get_singleton()->pressed, callable_mp(this, &DebugContextPill::_on_toggle_pressed));
	_header_row->add_child(_toggle_btn);

	// Make header row clickable to expand/collapse error list
	_pill_container->connect("gui_input", callable_mp(this, &DebugContextPill::_on_header_clicked));

	// Expandable error list (hidden by default)
	_error_list = memnew(VBoxContainer);
	_error_list->set_visible(false);
	_error_list->add_theme_constant_override("separation", 2 * EDSCALE);
	pill_vbox->add_child(_error_list);
}

// ============================================================================
// AIStatusIndicator - The colored circle showing connection status
// ============================================================================

void AIStatusIndicator::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_DRAW: {
			// Draw a filled circle
			Color indicator_color;
			switch (current_status) {
				case STATUS_UNKNOWN:
					indicator_color = AIColors::TEXT_MUTED;
					break;
				case STATUS_CHECKING:
					indicator_color = AIColors::WARNING;
					break;
				case STATUS_CONNECTED:
					indicator_color = AIColors::SUCCESS;
					break;
				case STATUS_DISCONNECTED:
					indicator_color = AIColors::ERROR;
					break;
			}

			Vector2 center = get_size() / 2.0;
			float radius = MIN(center.x, center.y) - 2.0;
			draw_circle(center, radius, indicator_color, true, -1.0, true); // antialiased
		} break;
	}
}

void AIStatusIndicator::_bind_methods() {
	BIND_ENUM_CONSTANT(STATUS_UNKNOWN);
	BIND_ENUM_CONSTANT(STATUS_CHECKING);
	BIND_ENUM_CONSTANT(STATUS_CONNECTED);
	BIND_ENUM_CONSTANT(STATUS_DISCONNECTED);
}

void AIStatusIndicator::set_status(Status p_status) {
	if (current_status != p_status) {
		current_status = p_status;
		queue_redraw();
	}
}

AIStatusIndicator::Status AIStatusIndicator::get_status() const {
	return current_status;
}

// ============================================================
// AITodoPanelWidget
// ============================================================

AITodoPanelWidget::AITodoPanelWidget() {
	// Outer panel styling
	Ref<StyleBoxFlat> panel_style;
	panel_style.instantiate();
	panel_style->set_bg_color(AIColors::BG_1);
	panel_style->set_border_color(AIColors::BORDER);
	panel_style->set_border_width_all(1);
	panel_style->set_corner_radius_all(AIColors::CORNER_RADIUS_SM * EDSCALE);
	panel_style->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
	add_theme_style_override("panel", panel_style);
	set_h_size_flags(SIZE_EXPAND_FILL);

	VBoxContainer *root_vbox = memnew(VBoxContainer);
	root_vbox->add_theme_constant_override("separation", AIColors::PADDING_XS * EDSCALE);
	add_child(root_vbox);

	// Header row
	HBoxContainer *header_row = memnew(HBoxContainer);
	root_vbox->add_child(header_row);

	Label *tasks_label = memnew(Label);
	tasks_label->set_text(TTR("Tasks"));
	tasks_label->add_theme_font_size_override("font_size", 11 * EDSCALE);
	tasks_label->add_theme_color_override("font_color", AIColors::TEXT_MUTED);
	tasks_label->set_h_size_flags(SIZE_EXPAND_FILL);
	header_row->add_child(tasks_label);

	progress_label = memnew(Label);
	progress_label->add_theme_font_size_override("font_size", 11 * EDSCALE);
	progress_label->add_theme_color_override("font_color", AIColors::TEXT_MUTED);
	header_row->add_child(progress_label);

	// Items container
	items_container = memnew(VBoxContainer);
	items_container->add_theme_constant_override("separation", AIColors::PADDING_XS * EDSCALE);
	root_vbox->add_child(items_container);
}

void AITodoPanelWidget::update_todos(const Array &p_todos) {
	// Clear existing items
	while (items_container->get_child_count() > 0) {
		Node *child = items_container->get_child(0);
		items_container->remove_child(child);
		memdelete(child);
	}

	// Count completed
	int completed = 0;
	for (int i = 0; i < p_todos.size(); i++) {
		if (p_todos[i].get_type() == Variant::DICTIONARY) {
			Dictionary item = p_todos[i];
			if (String(item.get("status", "")) == "completed") {
				completed++;
			}
		}
	}

	// Update progress label
	if (progress_label) {
		progress_label->set_text(vformat("%d/%d done", completed, p_todos.size()));
	}

	// Rebuild items
	for (int i = 0; i < p_todos.size(); i++) {
		if (p_todos[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		Dictionary item = p_todos[i];
		String status = item.get("status", "pending");
		String content = item.get("content", "");

		HBoxContainer *row = memnew(HBoxContainer);
		row->add_theme_constant_override("separation", 6 * EDSCALE);
		items_container->add_child(row);

		// Status icon
		Label *icon_label = memnew(Label);
		if (status == "completed") {
			icon_label->set_text(U"\u2713"); // ✓
			icon_label->add_theme_color_override("font_color", AIColors::SUCCESS);
		} else if (status == "in_progress") {
			icon_label->set_text(U"\u2192"); // →
			icon_label->add_theme_color_override("font_color", AIColors::ACCENT_BLUE);
		} else {
			icon_label->set_text(U"\u00b7"); // ·
			icon_label->add_theme_color_override("font_color", AIColors::TEXT_MUTED);
		}
		icon_label->add_theme_font_size_override("font_size", 12 * EDSCALE);
		row->add_child(icon_label);

		// Content label
		Label *content_label = memnew(Label);
		content_label->set_text(content);
		content_label->add_theme_font_size_override("font_size", 12 * EDSCALE);
		content_label->set_h_size_flags(SIZE_EXPAND_FILL);
		if (status == "completed") {
			content_label->add_theme_color_override("font_color", AIColors::TEXT_DISABLED);
		} else {
			content_label->add_theme_color_override("font_color", AIColors::TEXT_PRIMARY);
		}
		row->add_child(content_label);
	}
}

// ============================================================

AIStatusIndicator::AIStatusIndicator() {
	set_custom_minimum_size(Size2(12, 12) * EDSCALE);
	set_mouse_filter(MOUSE_FILTER_PASS);
	set_v_size_flags(SIZE_SHRINK_CENTER);
	// ColorRect's own fill must be transparent or it paints a white square
	// under the status circle.
	set_color(Color(0, 0, 0, 0));
}

// ============================================================================
// AIStatusPanel - The chat panel containing transcript and input
// ============================================================================

void AIStatusPanel::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			// Pick the most recent existing chat, or create a fresh one
			if (chat_store.is_valid()) {
				Vector<String> ids = AIChatStore::list_chat_ids();
				if (!ids.is_empty()) {
					chat_store->set_file_path(AIChatStore::make_chat_path(ids[0]));
				} else {
					chat_store->set_file_path(AIChatStore::make_chat_path(AIChatStore::generate_chat_id()));
				}
				chat_store->load_items();
				_rebuild_message_list();
				_refresh_context_usage();
			}
			// Initial connectivity check
			check_api_connectivity();

			// Start the debug pill polling timer now that we're in the tree
			if (debug_pill_update_timer) {
				debug_pill_update_timer->start();
			}

			// Track scroll position (user scrolls) and range changes (content added)
			ScrollBar *vbar = transcript_scroll->get_v_scroll_bar();
			vbar->connect("value_changed", callable_mp(this, &AIStatusPanel::_on_vscroll_changed));
			vbar->connect("changed", callable_mp(this, &AIStatusPanel::_on_scrollbar_range_changed));

			// Connect to AI provider signal
			if (Engine::get_singleton()->has_singleton("AI")) {
				Object *ai_obj = Engine::get_singleton()->get_singleton_object("AI");
				AI *ai = Object::cast_to<AI>(ai_obj);
				if (ai) {
					Ref<AIProvider> provider = ai->get_provider();
					if (provider.is_valid()) {
						if (!provider->is_connected("request_completed", callable_mp(this, &AIStatusPanel::_on_ai_response))) {
							provider->connect("request_completed", callable_mp(this, &AIStatusPanel::_on_ai_response));
						}
					}

					// Connect to orchestrator signals for agentic tool use
					Ref<AgenticOrchestrator> orchestrator = ai->get_orchestrator();
					if (orchestrator.is_valid()) {
						if (!orchestrator->is_connected("run_started", callable_mp(this, &AIStatusPanel::_on_orchestrator_started))) {
							orchestrator->connect("run_started", callable_mp(this, &AIStatusPanel::_on_orchestrator_started));
						}
						if (!orchestrator->is_connected("progress_update", callable_mp(this, &AIStatusPanel::_on_orchestrator_progress))) {
							orchestrator->connect("progress_update", callable_mp(this, &AIStatusPanel::_on_orchestrator_progress));
						}
						if (!orchestrator->is_connected("tool_result_ready", callable_mp(this, &AIStatusPanel::_on_orchestrator_tool_result))) {
							orchestrator->connect("tool_result_ready", callable_mp(this, &AIStatusPanel::_on_orchestrator_tool_result));
						}
						if (!orchestrator->is_connected("run_complete", callable_mp(this, &AIStatusPanel::_on_orchestrator_complete))) {
							orchestrator->connect("run_complete", callable_mp(this, &AIStatusPanel::_on_orchestrator_complete));
						}
						if (!orchestrator->is_connected("checkpoint_recommended", callable_mp(this, &AIStatusPanel::_on_checkpoint_recommended))) {
							orchestrator->connect("checkpoint_recommended", callable_mp(this, &AIStatusPanel::_on_checkpoint_recommended));
						}
						if (!orchestrator->is_connected("assistant_item_ready", callable_mp(this, &AIStatusPanel::_on_orchestrator_assistant_item))) {
							orchestrator->connect("assistant_item_ready", callable_mp(this, &AIStatusPanel::_on_orchestrator_assistant_item));
						}
						if (!orchestrator->is_connected("narration_ready", callable_mp(this, &AIStatusPanel::_on_orchestrator_narration))) {
							orchestrator->connect("narration_ready", callable_mp(this, &AIStatusPanel::_on_orchestrator_narration));
						}
						if (!orchestrator->is_connected("todos_updated", callable_mp(this, &AIStatusPanel::_on_todos_updated))) {
							orchestrator->connect("todos_updated", callable_mp(this, &AIStatusPanel::_on_todos_updated));
						}
						if (!orchestrator->is_connected("api_round_started", callable_mp(this, &AIStatusPanel::_on_api_round_started))) {
							orchestrator->connect("api_round_started", callable_mp(this, &AIStatusPanel::_on_api_round_started));
						}
						if (!orchestrator->is_connected("turn_tokens_ready", callable_mp(this, &AIStatusPanel::_on_turn_tokens_ready))) {
							orchestrator->connect("turn_tokens_ready", callable_mp(this, &AIStatusPanel::_on_turn_tokens_ready));
						}
						if (!orchestrator->is_connected("scene_diff_ready", callable_mp(this, &AIStatusPanel::_on_scene_diff_ready))) {
							orchestrator->connect("scene_diff_ready", callable_mp(this, &AIStatusPanel::_on_scene_diff_ready));
						}
						if (!orchestrator->is_connected("user_injection_consumed", callable_mp(this, &AIStatusPanel::_on_user_injection_consumed))) {
							orchestrator->connect("user_injection_consumed", callable_mp(this, &AIStatusPanel::_on_user_injection_consumed));
						}
					}
				}
			}
		} break;

		case NOTIFICATION_THEME_CHANGED: {
			// Update button icons if needed
			_rebuild_message_list();
		} break;
	}
}

void AIStatusPanel::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_on_send_button_pressed"), &AIStatusPanel::_on_send_button_pressed);
	ClassDB::bind_method(D_METHOD("_on_delete_chat_confirmed"), &AIStatusPanel::_on_delete_chat_confirmed);
	ClassDB::bind_method(D_METHOD("_on_prompt_text_changed"), &AIStatusPanel::_on_prompt_text_changed);
	ClassDB::bind_method(D_METHOD("_on_queue_item_edit", "index"), &AIStatusPanel::_on_queue_item_edit);
	ClassDB::bind_method(D_METHOD("_on_queue_item_remove", "index"), &AIStatusPanel::_on_queue_item_remove);
	ClassDB::bind_method(D_METHOD("_on_ai_response", "success", "response", "error"), &AIStatusPanel::_on_ai_response);
	ClassDB::bind_method(D_METHOD("_on_orchestrator_started"), &AIStatusPanel::_on_orchestrator_started);
	ClassDB::bind_method(D_METHOD("_on_orchestrator_progress", "status", "turn"), &AIStatusPanel::_on_orchestrator_progress);
	ClassDB::bind_method(D_METHOD("_on_orchestrator_assistant_item", "item"), &AIStatusPanel::_on_orchestrator_assistant_item);
	ClassDB::bind_method(D_METHOD("_on_orchestrator_tool_result", "tool_result"), &AIStatusPanel::_on_orchestrator_tool_result);
	ClassDB::bind_method(D_METHOD("_on_orchestrator_complete", "success", "final_message"), &AIStatusPanel::_on_orchestrator_complete);
	ClassDB::bind_method(D_METHOD("_on_todos_updated", "todos"), &AIStatusPanel::_on_todos_updated);
	ClassDB::bind_method(D_METHOD("_on_api_round_started", "turn"), &AIStatusPanel::_on_api_round_started);
	ClassDB::bind_method(D_METHOD("_on_turn_tokens_ready", "tokens"), &AIStatusPanel::_on_turn_tokens_ready);
	ClassDB::bind_method(D_METHOD("_on_thinking_dot_tick"), &AIStatusPanel::_on_thinking_dot_tick);
	ClassDB::bind_method(D_METHOD("_on_debug_pill_update_tick"), &AIStatusPanel::_on_debug_pill_update_tick);
	ClassDB::bind_method(D_METHOD("_on_debug_context_toggled", "enabled"), &AIStatusPanel::_on_debug_context_toggled);
	ClassDB::bind_method(D_METHOD("_on_rewind_clicked", "message_id"), &AIStatusPanel::_on_rewind_clicked);
	ClassDB::bind_method(D_METHOD("_on_dialog_cancel"), &AIStatusPanel::_on_dialog_cancel);
	ClassDB::bind_method(D_METHOD("_on_dialog_continue_no_revert"), &AIStatusPanel::_on_dialog_continue_no_revert);
	ClassDB::bind_method(D_METHOD("_on_dialog_continue_revert"), &AIStatusPanel::_on_dialog_continue_revert);
	ClassDB::bind_method(D_METHOD("_on_edit_clicked", "message_id"), &AIStatusPanel::_on_edit_clicked);
	ClassDB::bind_method(D_METHOD("_on_checkpoint_recommended", "user_message_id"), &AIStatusPanel::_on_checkpoint_recommended);
	ClassDB::bind_method(D_METHOD("_on_openai_request_completed", "result", "response_code", "headers", "body"), &AIStatusPanel::_on_openai_request_completed);
	ClassDB::bind_method(D_METHOD("_on_gemini_request_completed", "result", "response_code", "headers", "body"), &AIStatusPanel::_on_gemini_request_completed);
	ClassDB::bind_method(D_METHOD("_on_xai_request_completed", "result", "response_code", "headers", "body"), &AIStatusPanel::_on_xai_request_completed);
	ClassDB::bind_method(D_METHOD("check_api_connectivity"), &AIStatusPanel::check_api_connectivity);
}

void AIStatusPanel::_rebuild_message_list() {
	if (!message_list) {
		return;
	}

	// Clear existing messages
	while (message_list->get_child_count() > 0) {
		Node *child = message_list->get_child(0);
		message_list->remove_child(child);
		memdelete(child);
	}
	pending_message = nullptr;
	// Any pointers in the map are dangling now — wipe before we repopulate.
	pending_tool_entries.clear();
	if (pending_tool_timer) {
		pending_tool_timer->stop();
	}

	if (chat_store.is_valid()) {
		const Vector<HistoryItem> &items = chat_store->get_items();

		// Track API round boundaries for separators and token totals.
		// A new round starts when we see an assistant item after tool results.
		int round_token_total = 0;
		bool seen_tools_in_round = false; // true once we've processed tool results in the current round

		for (int i = 0; i < items.size(); i++) {
			const HistoryItem &item = items[i];

			if (item.is_injection()) {
				// Render engine_state and parse_error_state as inline error bubbles
				String item_type = item.data.get("type", "");
				if (item_type == "engine_state") {
					Array errors = item.data.get("errors", Array());
					int error_count = item.data.get("error_count", 0);
					int warning_count = item.data.get("warning_count", 0);
					if (error_count > 0 || warning_count > 0) {
						String summary;
						if (error_count > 0 && warning_count > 0) {
							summary = vformat("Including %d error%s, %d warning%s",
									error_count, error_count == 1 ? "" : "s",
									warning_count, warning_count == 1 ? "" : "s");
						} else if (error_count > 0) {
							summary = vformat("Including %d error%s", error_count, error_count == 1 ? "" : "s");
						} else {
							summary = vformat("Including %d warning%s", warning_count, warning_count == 1 ? "" : "s");
						}
						Control *bubble = _create_error_bubble_impl(
								summary, errors,
								Color(0, 0, 0, 0),
								Color(0.7f, 0.3f, 0.3f, 0.6f),
								Color(1.0f, 0.5f, 0.45f, 1.0f),
								ERROR_BUBBLE_RUNTIME);
						message_list->add_child(bubble);
					}
				} else if (item_type == "thinking") {
					// Reasoning text persisted by the harness loop — rendered
					// with the same italic style as the live stream.
					String thinking_text = item.data.get("text", "");
					if (!thinking_text.is_empty()) {
						_append_thinking_ui(thinking_text);
					}
				} else if (item_type == "parse_error_state") {
					Array errors = item.data.get("errors", Array());
					String file_path = item.data.get("file_path", "");
					int error_count = item.data.get("error_count", 0);
					if (error_count > 0) {
						String summary = vformat("%d parse error%s in %s",
								error_count, error_count == 1 ? "" : "s",
								file_path.get_file());
						Control *bubble = _create_error_bubble_impl(
								summary, errors,
								Color(0, 0, 0, 0),
								Color(0.8f, 0.5f, 0.2f, 0.6f),
								Color(1.0f, 0.6f, 0.3f, 1.0f),
								ERROR_BUBBLE_PARSE);
						message_list->add_child(bubble);
					}
				}
				continue; // skip other injections (todo_state, etc.)
			}

			String role = item.role();

			if (role == "user") {
				// A user message after tool results means the previous run ended.
				// Insert the final token total for that run.
				if (seen_tools_in_round && round_token_total > 0) {
					_insert_token_total_label_into(message_list, round_token_total);
				}
				round_token_total = 0;
				seen_tools_in_round = false;

				String content = item.data.get("content", "");
				if (content.begins_with("<turn_cancelled>")) {
					// Cancelled run marker — render as bold inline notice, not a user bubble.
					Control *notice = _create_cancel_notice();
					if (notice) {
						message_list->add_child(notice);
					}
				} else {
					Control *bubble = _create_message_bubble(item);
					if (bubble) {
						message_list->add_child(bubble);
					}
				}

			} else if (role == "assistant") {
				// If we've seen tool results already, this is a new API round.
				// Insert token total + separator for the previous round.
				if (seen_tools_in_round) {
					if (round_token_total > 0) {
						_insert_token_total_label_into(message_list, round_token_total);
					}
					round_token_total = 0;
					seen_tools_in_round = false;

					// Visual divider between API rounds (matches live separator style)
					HSeparator *sep = memnew(HSeparator);
					sep->set_h_size_flags(SIZE_EXPAND_FILL);
					Ref<StyleBoxLine> style;
					style.instantiate();
					style->set_color(Color(1, 1, 1, 0.08f));
					style->set_thickness(1);
					sep->add_theme_style_override("separator", style);
					sep->add_theme_constant_override("separation", 6);
					message_list->add_child(sep);
				}

				// Render text blocks with the shared assistant renderer — same
				// borderless rich text (and plan panels) as live runs.
				Array content = item.data.get("content", Array());
				for (int j = 0; j < content.size(); j++) {
					Dictionary block = content[j];
					if (String(block.get("type", "")) == "text") {
						String text = block.get("text", "");
						if (!text.is_empty()) {
							_append_assistant_blocks(text, nullptr);
							break; // one text item per assistant item
						}
					}
				}
				// Create a ToolCollapsibleEntry for each tool_call block
				for (int j = 0; j < content.size(); j++) {
					Dictionary block = content[j];
					if (String(block.get("type", "")) != "tool_call") {
						continue;
					}
					String call_id = block.get("id", "");
					String tool_name = block.get("name", "");
					// update_todos never gets a transcript card (todo panel instead) —
					// without this skip it renders as a forever-pending card.
					if (tool_name == "update_todos") {
						continue;
					}
					Dictionary args = block.get("args", Dictionary());

					Dictionary placeholder;
					placeholder["type"] = tool_name;
					placeholder["tool_name"] = tool_name;
					placeholder["args"] = args;
					placeholder["action_id"] = call_id;
					placeholder["status"] = "pending";
					placeholder["tokens"] = 0;

					ToolCollapsibleEntry *entry = Object::cast_to<ToolCollapsibleEntry>(_create_tool_result_ui(placeholder));
					if (entry) {
						entry->set_token_label_visible(_show_token_counts);
						message_list->add_child(entry);
						if (!call_id.is_empty()) {
							pending_tool_entries[call_id] = entry;
						}
					}
				}

			} else if (role == "tool") {
				String call_id = item.data.get("tool_call_id", "");
				Dictionary content_dict = item.data.get("content", Dictionary());

				// update_todos is persisted for API-history correctness but shown in
				// the todo panel, not the transcript (mirrors the live suppression).
				if (String(content_dict.get("tool_name", "")) == "update_todos") {
					continue;
				}

				// Estimate tokens for this tool result (same heuristic as orchestrator)
				int tokens;
				String tn = content_dict.get("tool_name", "");
				if (tn == "run_and_screenshot" || tn == "capture_2d_viewport" || tn == "capture_3d_viewport") {
					tokens = 1000;
				} else {
					tokens = JSON::stringify(content_dict).length() / 4;
				}
				round_token_total += tokens;
				seen_tools_in_round = true;

				// Build display dict from canonical tool item
				Dictionary display;
				String tool_name = content_dict.get("tool_name", "");
				display["type"] = tool_name;
				display["tool_name"] = tool_name;
				display["args"] = content_dict.get("args", Dictionary());
				display["action_id"] = call_id;
				display["status"] = content_dict.get("status", "error");
				if (content_dict.has("result")) {
					Dictionary result = content_dict["result"];
					// Reload persisted screenshot(s) for UI thumbnail. Both the
					// single-shot and multi-shot shapes need rehydration —
					// `update_from_tool_result` reads `screenshot_b64` /
					// `screenshots[].screenshot_b64`, so we restore those fields
					// from disk before handing the dict on.
					if (chat_store.is_valid()) {
						bool needs_dup = result.has("screenshot") || result.has("screenshots");
						if (needs_dup) {
							result = result.duplicate();
						}
						if (result.has("screenshot")) {
							String b64 = chat_store->load_screenshot_b64(result["screenshot"]);
							if (!b64.is_empty()) {
								result["screenshot_b64"] = b64;
							}
						}
						if (result.has("screenshots") && result["screenshots"].get_type() == Variant::ARRAY) {
							Array shots = result["screenshots"];
							for (int si = 0; si < shots.size(); si++) {
								Dictionary entry = shots[si];
								if (entry.has("screenshot")) {
									String b64 = chat_store->load_screenshot_b64(entry["screenshot"]);
									if (!b64.is_empty()) {
										entry["screenshot_b64"] = b64;
										shots[si] = entry;
									}
								}
							}
							result["screenshots"] = shots;
						}
					}
					display["result"] = result;
				}
				if (content_dict.has("error")) {
					display["error"] = content_dict["error"];
				}
				display["tokens"] = tokens;

				// Update paired entry if found, otherwise create standalone
				if (!call_id.is_empty() && pending_tool_entries.has(call_id)) {
					ToolCollapsibleEntry *paired = pending_tool_entries[call_id];
					paired->update_from_tool_result(display);
					// Wire stack-click → lightbox the same way the live and
					// non-pending paths do; without this, screenshots loaded
					// from a previous session render but never respond to a
					// click. Mirrors the connect at line ~1909 / ~1930.
					if (paired->get_image_stack() && paired->get_image_stack()->get_image_count() > 0) {
						paired->get_image_stack()->connect("clicked",
								callable_mp(this, &AIStatusPanel::_show_image_popup).bind(paired->get_screenshot_b64s()));
					}
					pending_tool_entries.erase(call_id);
				} else {
					Control *ui = _create_tool_result_ui(display);
					if (ui) {
						message_list->add_child(ui);
					}
				}
			} else if (role == "system") {
				String type = item.data.get("type", "");
				if (type == "error") {
					String content = item.data.get("content", "");
					Control *notice = _create_error_notice(content);
					if (notice) {
						message_list->add_child(notice);
					}
				}
			}
		}

		// Insert final token total for the last round (run ended without another user/assistant message)
		if (seen_tools_in_round && round_token_total > 0) {
			_insert_token_total_label_into(message_list, round_token_total);
		}
	}

	// Orphaned pending cards from unclean shutdowns (assistant tool_calls with no
	// matching tool result in the log) render as static, un-animated cards. We
	// clear the map so a subsequent live run doesn't try to pair against them or
	// kick the spinner timer.
	pending_tool_entries.clear();

	// Reset auto-scroll so the rebuild always lands at the bottom
	should_auto_scroll = true;
}

void AIStatusPanel::_append_message_ui(const HistoryItem &p_item) {
	if (!message_list) {
		return;
	}

	Control *bubble = _create_message_bubble(p_item);
	if (bubble) {
		message_list->add_child(bubble);
	}
}

// ============================================================================
// Markdown → BBCode conversion for chat message rendering
// ============================================================================

// Processes inline Markdown within a single line: **bold**, *italic*, `code`.
// Escapes literal [ characters so they don't trigger BBCode parsing.
static String _process_inline_md(const String &p_text) {
	String out;
	int i = 0;
	int n = p_text.length();

	while (i < n) {
		char32_t c = p_text[i];

		// Inline code `...` — escape brackets inside, no further processing
		if (c == '`') {
			int end = p_text.find("`", i + 1);
			if (end > i) {
				String inner = p_text.substr(i + 1, end - i - 1).replace("[", "[lb]");
				out += "[code]" + inner + "[/code]";
				i = end + 1;
				continue;
			}
		}

		// Bold **text** — check before italic so ** isn't consumed as two *
		if (c == '*' && i + 1 < n && p_text[i + 1] == '*') {
			int end = p_text.find("**", i + 2);
			if (end > i + 1) {
				out += "[b]" + _process_inline_md(p_text.substr(i + 2, end - i - 2)) + "[/b]";
				i = end + 2;
				continue;
			}
		}

		// Italic *text* — single *, not part of **
		if (c == '*' && (i + 1 >= n || p_text[i + 1] != '*')) {
			// Find closing * that isn't part of **
			int end = -1;
			for (int j = i + 1; j < n; j++) {
				if (p_text[j] == '*' && (j + 1 >= n || p_text[j + 1] != '*')) {
					end = j;
					break;
				}
			}
			if (end > i) {
				out += "[i]" + _process_inline_md(p_text.substr(i + 1, end - i - 1)) + "[/i]";
				i = end + 1;
				continue;
			}
		}

		// Escape [ so it doesn't trigger BBCode tags
		if (c == '[') {
			out += "[lb]";
			i++;
			continue;
		}

		out += String::chr(c);
		i++;
	}
	return out;
}

// Converts a Markdown string to Godot BBCode for RichTextLabel rendering.
// Handles: fenced code blocks, headers (#/##/###), bullet lists (- * +),
// ordered lists, horizontal rules, bold, italic, and inline code.
static String _markdown_to_bbcode(const String &p_markdown) {
	String out;
	PackedStringArray lines = p_markdown.split("\n");
	bool in_code_block = false;

	for (int li = 0; li < lines.size(); li++) {
		String line = lines[li];
		bool last_line = (li == lines.size() - 1);

		// Fenced code block open/close
		if (line.begins_with("```")) {
			if (!in_code_block) {
				in_code_block = true;
				out += "[code]";
			} else {
				in_code_block = false;
				out += "[/code]";
			}
			if (!last_line) {
				out += "\n";
			}
			continue;
		}

		// Inside code block — only escape brackets, no Markdown processing
		if (in_code_block) {
			out += line.replace("[", "[lb]");
			if (!last_line) {
				out += "\n";
			}
			continue;
		}

		// Headers
		if (line.begins_with("### ")) {
			out += "[b]" + _process_inline_md(line.substr(4)) + "[/b]";
		} else if (line.begins_with("## ")) {
			out += "[b]" + _process_inline_md(line.substr(3)) + "[/b]";
		} else if (line.begins_with("# ")) {
			out += "[b]" + _process_inline_md(line.substr(2)) + "[/b]";
		}
		// Unordered list: - / * / +
		else if (line.begins_with("- ") || line.begins_with("* ") || line.begins_with("+ ")) {
			out += String::utf8("  • ") + _process_inline_md(line.substr(2));
		}
		// Ordered list: 1. / 2. / etc.
		else if (line.length() >= 3 && line[0] >= '1' && line[0] <= '9' && line[1] == '.' && line[2] == ' ') {
			out += String::chr(line[0]) + ". " + _process_inline_md(line.substr(3));
		}
		// Horizontal rule
		else if (line == "---" || line == "***" || line == "___") {
			out += "[color=#444444]" + String::utf8("────────────────────────────────") + "[/color]";
		}
		// Regular line
		else {
			out += _process_inline_md(line);
		}

		if (!last_line) {
			out += "\n";
		}
	}

	return out;
}

// Renders message text into a RichTextLabel, detecting data:image/ URIs and inserting
// them inline using add_image() so they render as images rather than base64 text.
static void _append_message_content(RichTextLabel *p_label, const String &p_text) {
	String remaining = p_text;
	while (true) {
		int marker = remaining.find("data:image/");
		if (marker == -1) {
			p_label->append_text(_markdown_to_bbcode(remaining));
			break;
		}
		if (marker > 0) {
			p_label->append_text(_markdown_to_bbcode(remaining.substr(0, marker)));
		}
		// Find end of data URI — stop at whitespace, quote, or closing paren
		int end = marker + 11;
		while (end < remaining.length()) {
			char32_t c = remaining[end];
			if (c == ' ' || c == '\n' || c == '\r' || c == '"' || c == '\'' || c == ')') {
				break;
			}
			end++;
		}
		String data_uri = remaining.substr(marker, end - marker);
		remaining = remaining.substr(end);

		int comma = data_uri.find(",");
		if (comma != -1) {
			String b64 = data_uri.substr(comma + 1);
			PackedByteArray bytes = CoreBind::Marshalls::get_singleton()->base64_to_raw(b64);
			if (!bytes.is_empty()) {
				Ref<Image> img;
				img.instantiate();
				if (img->load_png_from_buffer(bytes) == OK && !img->is_empty()) {
					p_label->add_image(ImageTexture::create_from_image(img), 0, 200 * EDSCALE);
				}
			}
		}
	}
}

Control *AIStatusPanel::_create_message_bubble(const HistoryItem &p_item) {
	String role = p_item.role();

	// Extract display content:
	// - user items: data["content"] is a plain string
	// - assistant items: concatenate text blocks from data["content"] array
	String display_content;
	if (role == "user") {
		display_content = p_item.data.get("content", "");
		// Paragraph spacing for user newlines, matching the visual rhythm of
		// assistant paragraphs (single breaks read cramped in the bubble).
		display_content = display_content.replace("\r\n", "\n").replace("\n", "\n\n");
		while (display_content.contains("\n\n\n")) {
			display_content = display_content.replace("\n\n\n", "\n\n");
		}
	} else if (role == "assistant") {
		Array content_blocks = p_item.data.get("content", Array());
		for (int j = 0; j < content_blocks.size(); j++) {
			Dictionary block = content_blocks[j];
			if (String(block.get("type", "")) == "text") {
				if (!display_content.is_empty()) {
					display_content += "\n";
				}
				display_content += String(block.get("text", ""));
			}
		}
	}

	// Create container for alignment
	HBoxContainer *align_container = memnew(HBoxContainer);
	align_container->set_h_size_flags(SIZE_EXPAND_FILL);

	// Create the bubble panel
	PanelContainer *bubble = memnew(PanelContainer);

	// Style the bubble based on role
	Ref<StyleBoxFlat> style;
	style.instantiate();
	style->set_corner_radius_all(AIColors::CORNER_RADIUS_LG * EDSCALE);
	style->set_content_margin_all(AIColors::PADDING_MD * EDSCALE);

	bool is_user = role == "user";

	if (is_user) {
		// User messages: transparent with accent outline, right aligned
		style->set_bg_color(Color(0, 0, 0, 0));
		style->set_border_width_all(1);
		style->set_border_color(AIColors::BORDER_LIGHT);
		// Spacer on left pushes bubble right (preserves visual distinction from assistant)
		Control *spacer = memnew(Control);
		spacer->set_h_size_flags(SIZE_EXPAND_FILL);
		spacer->set_stretch_ratio(0.15);
		align_container->add_child(spacer);
		bubble->set_h_size_flags(SIZE_EXPAND_FILL);
		bubble->set_stretch_ratio(0.85);
		align_container->add_child(bubble);
	} else {
		// Assistant messages: distinct dark with subtle border, left aligned
		style->set_bg_color(AIColors::ASSISTANT_BG);
		style->set_border_width_all(1);
		style->set_border_color(AIColors::BORDER);
		bubble->set_h_size_flags(SIZE_EXPAND_FILL);
		bubble->set_stretch_ratio(0.95); // Bubble takes up to 95%
		align_container->add_child(bubble);
		// Add small spacer on right
		Control *spacer = memnew(Control);
		spacer->set_h_size_flags(SIZE_EXPAND_FILL);
		spacer->set_stretch_ratio(0.05);
		align_container->add_child(spacer);
	}

	bubble->add_theme_style_override("panel", style);

	// Create inner VBox to hold header (for user messages) and content
	VBoxContainer *inner_vbox = memnew(VBoxContainer);
	inner_vbox->add_theme_constant_override("separation", AIColors::PADDING_XS * EDSCALE);
	// PASS so right-clicks on the vbox's whitespace (between header and label,
	// or margins around the label) propagate up to the bubble's gui_input.
	inner_vbox->set_mouse_filter(Control::MOUSE_FILTER_PASS);
	bubble->add_child(inner_vbox);

	// For user messages, add a header row with edit and rewind buttons
	if (is_user && p_item.ts != 0) {
		HBoxContainer *header_row = memnew(HBoxContainer);
		header_row->set_h_size_flags(SIZE_EXPAND_FILL);
		// PASS so right-clicks in the header whitespace (next to the buttons)
		// propagate up to the bubble's gui_input.
		header_row->set_mouse_filter(Control::MOUSE_FILTER_PASS);
		inner_vbox->add_child(header_row);

		// Rewind button (↶ unicode character) — left side
		Button *rewind_btn = memnew(Button);
		rewind_btn->set_flat(true);
		rewind_btn->set_text(String::utf8("↶"));
		rewind_btn->set_tooltip_text(TTR("Rewind to this message"));
		rewind_btn->set_custom_minimum_size(Size2(18, 18) * EDSCALE);
		rewind_btn->add_theme_color_override("font_color", AIColors::TEXT_MUTED);
		rewind_btn->add_theme_color_override("font_hover_color", AIColors::ACCENT_BLUE);
		rewind_btn->add_theme_color_override("font_pressed_color", AIColors::ACCENT_BLUE_PRESSED);
		rewind_btn->set_meta("message_id", p_item.ts);
		rewind_btn->connect(SceneStringNames::get_singleton()->pressed, callable_mp(this, &AIStatusPanel::_on_rewind_clicked).bind(p_item.ts));
		header_row->add_child(rewind_btn);

		// Edit button (✎ pencil unicode character)
		Button *edit_btn = memnew(Button);
		edit_btn->set_flat(true);
		edit_btn->set_text(String::utf8("✎"));
		edit_btn->set_tooltip_text(TTR("Edit and resend this message"));
		edit_btn->set_custom_minimum_size(Size2(18, 18) * EDSCALE);
		edit_btn->add_theme_color_override("font_color", AIColors::TEXT_MUTED);
		edit_btn->add_theme_color_override("font_hover_color", AIColors::ACCENT_BLUE);
		edit_btn->add_theme_color_override("font_pressed_color", AIColors::ACCENT_BLUE_PRESSED);
		edit_btn->set_meta("message_id", p_item.ts);
		edit_btn->connect(SceneStringNames::get_singleton()->pressed, callable_mp(this, &AIStatusPanel::_on_edit_clicked).bind(p_item.ts));
		header_row->add_child(edit_btn);

		// Flexible spacer to fill remaining space on the right. PASS so a
		// right-click on the empty header area still reaches the bubble.
		Control *header_spacer = memnew(Control);
		header_spacer->set_h_size_flags(SIZE_EXPAND_FILL);
		header_spacer->set_mouse_filter(Control::MOUSE_FILTER_PASS);
		header_row->add_child(header_spacer);
	}

	// Create label for content
	RichTextLabel *label = memnew(RichTextLabel);
	label->set_use_bbcode(true);
	label->set_fit_content(true);
	label->set_scroll_active(false);
	label->set_selection_enabled(true);
	label->add_theme_color_override("default_color", AIColors::TEXT_PRIMARY);
	label->add_theme_color_override("background_color", Color(0, 0, 0, 0));
	label->add_theme_color_override("selection_color", Color(0.3f, 0.6f, 1.0f, 0.3f));
	// Remove the editor theme's StyleBox so it doesn't draw its own border/bg on top of the panel
	Ref<StyleBoxEmpty> label_empty_style;
	label_empty_style.instantiate();
	label->add_theme_style_override("normal", label_empty_style);
	label->add_theme_style_override("focus", label_empty_style);

	// Display content (with inline image support for data: URIs)
	_append_message_content(label, display_content);

	inner_vbox->add_child(label);

	// Assistant bubbles: append a muted latency footer ("openai · 1.4s").
	// Visibility mirrors the Debug toggle (_show_token_counts).
	if (!is_user && p_item.data.has("latency_ms")) {
		int64_t lat_ms = p_item.data.get("latency_ms", 0);
		if (lat_ms > 0) {
			String prov = p_item.data.get("provider", "");
			String formatted;
			if (lat_ms < 1000) {
				formatted = itos(lat_ms) + "ms";
			} else {
				double secs = (double)lat_ms / 1000.0;
				formatted = String::num(secs, 1) + "s";
			}
			String footer_text = prov.is_empty() ? formatted : prov + " · " + formatted;
			Label *latency_lbl = memnew(Label);
			latency_lbl->set_text(footer_text);
			latency_lbl->add_theme_color_override("font_color", AIColors::TEXT_MUTED);
			latency_lbl->add_theme_font_size_override("font_size", 10 * EDSCALE);
			latency_lbl->set_mouse_filter(Control::MOUSE_FILTER_PASS);
			latency_lbl->set_meta("_ai_latency_label", true);
			latency_lbl->set_visible(_show_token_counts);
			inner_vbox->add_child(latency_lbl);
		}
	}

	// Right-click → show "Copy text" context menu. Stash the plain text on the
	// bubble and listen on both the bubble (for margin clicks) and the label
	// (which consumes events inside its own rect). The label's STOP filter
	// would otherwise swallow the right-click before it reaches the bubble.
	bubble->set_meta("_bubble_plain_text", display_content);
	bubble->set_mouse_filter(Control::MOUSE_FILTER_STOP);
	bubble->connect("gui_input", callable_mp(this, &AIStatusPanel::_on_message_bubble_gui_input).bind(bubble));
	label->connect("gui_input", callable_mp(this, &AIStatusPanel::_on_message_bubble_gui_input).bind(bubble));

	// Image thumbnails for user messages (if any)
	if (is_user && p_item.data.has("images")) {
		Array images = p_item.data.get("images", Array());
		if (!images.is_empty()) {
			HBoxContainer *img_row = memnew(HBoxContainer);
			img_row->add_theme_constant_override("separation", AIColors::PADDING_XS * EDSCALE);
			inner_vbox->add_child(img_row);

			for (int i = 0; i < images.size(); i++) {
				String b64 = images[i];
				// Decode base64 → PNG bytes → Image → ImageTexture
				PackedByteArray png_bytes = CoreBind::Marshalls::get_singleton()->base64_to_raw(b64);
				if (png_bytes.is_empty()) {
					continue;
				}
				Ref<Image> img;
				img.instantiate();
				Error err = img->load_png_from_buffer(png_bytes);
				if (err != OK || img->is_empty()) {
					continue;
				}
				Ref<ImageTexture> img_tex = ImageTexture::create_from_image(img);
				TextureRect *tex = memnew(TextureRect);
				tex->set_texture(img_tex);
				tex->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_COVERED);
				tex->set_custom_minimum_size(Size2(80, 80) * EDSCALE);
				tex->set_mouse_filter(Control::MOUSE_FILTER_STOP);
				tex->set_default_cursor_shape(Control::CURSOR_POINTING_HAND);
				tex->connect("gui_input", callable_mp(this, &AIStatusPanel::_on_thumbnail_gui_input).bind(b64));
				img_row->add_child(tex);
			}
		}
	}

	return align_container;
}

Control *AIStatusPanel::_create_tool_result_ui(const Dictionary &p_tool_result) {
	// Create collapsible entry for tool result (collapsed by default)
	ToolCollapsibleEntry *entry = memnew(ToolCollapsibleEntry);
	entry->update_from_tool_result(p_tool_result);
	entry->set_token_label_visible(_show_token_counts);
	// Wire screenshot click → lightbox popup (same as chat image thumbnails)
	if (entry->get_image_stack() && entry->get_image_stack()->get_image_count() > 0) {
		entry->get_image_stack()->connect("clicked",
				callable_mp(this, &AIStatusPanel::_show_image_popup).bind(entry->get_screenshot_b64s()));
	}
	return entry;
}

void AIStatusPanel::_append_tool_result_ui(const Dictionary &p_tool_result) {
	if (!message_list) {
		return;
	}

	// If the assistant item already pre-created a pending card for this tool_call,
	// upgrade it in place instead of appending a duplicate.
	String call_id = p_tool_result.get("action_id", "");
	if (!call_id.is_empty() && pending_tool_entries.has(call_id)) {
		ToolCollapsibleEntry *entry = pending_tool_entries[call_id];
		if (entry) {
			entry->update_from_tool_result(p_tool_result);
			entry->set_token_label_visible(_show_token_counts);
			// Wire image click, now that a screenshot may exist
			if (entry->get_image_stack() && entry->get_image_stack()->get_image_count() > 0) {
				entry->get_image_stack()->connect("clicked",
						callable_mp(this, &AIStatusPanel::_show_image_popup).bind(entry->get_screenshot_b64s()));
			}
		}
		pending_tool_entries.erase(call_id);
		if (pending_tool_entries.is_empty() && pending_tool_timer) {
			pending_tool_timer->stop();
		}
		return;
	}

	Control *tool_result_ui = _create_tool_result_ui(p_tool_result);
	if (tool_result_ui) {
		message_list->add_child(tool_result_ui);
		if (pending_message) {
			message_list->move_child(tool_result_ui, pending_message->get_index());
		}
	}
}

void AIStatusPanel::_on_scrollbar_range_changed() {
	if (should_auto_scroll) {
		_scroll_to_bottom();
	}
}

void AIStatusPanel::_on_vscroll_changed(float p_value) {
	if (!transcript_scroll) {
		return;
	}
	ScrollBar *vbar = transcript_scroll->get_v_scroll_bar();
	float max_scroll = vbar->get_max() - vbar->get_page();
	should_auto_scroll = (p_value >= max_scroll - 10.0f);
}

void AIStatusPanel::_scroll_to_bottom() {
	if (transcript_scroll) {
		transcript_scroll->set_v_scroll(transcript_scroll->get_v_scroll_bar()->get_max());
	}
}

void AIStatusPanel::_update_send_button_state() {
	if (!send_button || !prompt_edit) {
		return;
	}

	// Button appearance and behavior depends on run state
	switch (run_state) {
		case STATE_IDLE: {
			// Send mode: enabled if there's text or images to send (and context not exhausted)
			send_button->set_text(TTR("Send"));
			// Clear any overrides set by Stop/Stopping states
			send_button->remove_theme_style_override("pressed");
			send_button->remove_theme_font_size_override("font_size");
			send_button->remove_theme_color_override("font_color");
			send_button->remove_theme_color_override("font_hover_color");
			if (context_exhausted) {
				send_button->set_disabled(true);
				send_button->set_tooltip_text(TTR("Context is full. Start a new chat (+) to continue."));
				break;
			}
			send_button->set_tooltip_text("");
			bool has_text = !prompt_edit->get_text().strip_edges().is_empty();
			bool has_images = !pending_images.is_empty();
			send_button->set_disabled(!has_text && !has_images);

			// Apply accent blue style for Send
			Ref<StyleBoxFlat> send_normal;
			send_normal.instantiate();
			send_normal->set_bg_color(AIColors::ACCENT_BLUE);
			send_normal->set_corner_radius_all(AIColors::CORNER_RADIUS_MD * EDSCALE);
			send_normal->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
			send_button->add_theme_style_override("normal", send_normal);

			Ref<StyleBoxFlat> send_hover;
			send_hover.instantiate();
			send_hover->set_bg_color(AIColors::ACCENT_BLUE_HOVER);
			send_hover->set_corner_radius_all(AIColors::CORNER_RADIUS_MD * EDSCALE);
			send_hover->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
			send_button->add_theme_style_override("hover", send_hover);
		} break;

		case STATE_RUNNING: {
			// Stop: plain flat text, no bubble background — styled like the Thinking indicator but bigger
			send_button->set_text(TTR("Stop"));
			send_button->set_disabled(false);

			Ref<StyleBoxEmpty> stop_empty;
			stop_empty.instantiate();
			send_button->add_theme_style_override("normal", stop_empty);
			send_button->add_theme_style_override("hover", stop_empty);
			send_button->add_theme_style_override("pressed", stop_empty);
			send_button->add_theme_color_override("font_color", AIColors::TEXT_SECONDARY);
			send_button->add_theme_color_override("font_hover_color", AIColors::TEXT_PRIMARY);
		} break;
	}
}

void AIStatusPanel::_update_queue_ui() {
	if (!queue_container) {
		return;
	}

	if (message_queue.is_empty()) {
		queue_container->set_visible(false);
	} else {
		queue_container->set_visible(true);
		if (queue_header_label) {
			queue_header_label->set_text(vformat(TTR("Queued (%d)"), message_queue.size()));
		}
		_rebuild_queue_list();
	}
}

void AIStatusPanel::_rebuild_queue_list() {
	if (!queue_container) {
		return;
	}

	// Clear existing items (skip the header label at index 0)
	while (queue_container->get_child_count() > 1) {
		Node *child = queue_container->get_child(1);
		queue_container->remove_child(child);
		memdelete(child);
	}

	// Add items for each queued message
	for (int i = 0; i < message_queue.size(); i++) {
		Control *item = _create_queue_item(i, message_queue[i]);
		if (item) {
			queue_container->add_child(item);
		}
	}
}

Control *AIStatusPanel::_create_queue_item(int p_index, const QueuedMessage &p_msg) {
	// Main container for the queue item
	PanelContainer *item_panel = memnew(PanelContainer);
	item_panel->set_h_size_flags(SIZE_EXPAND_FILL);

	// Style the item panel
	Ref<StyleBoxFlat> item_style;
	item_style.instantiate();
	item_style->set_bg_color(AIColors::BG_1);
	item_style->set_border_width_all(1);
	item_style->set_border_color(AIColors::BORDER);
	item_style->set_corner_radius_all(AIColors::CORNER_RADIUS_SM * EDSCALE);
	item_style->set_content_margin(SIDE_LEFT, AIColors::PADDING_SM * EDSCALE);
	item_style->set_content_margin(SIDE_RIGHT, AIColors::PADDING_SM * EDSCALE);
	item_style->set_content_margin(SIDE_TOP, AIColors::PADDING_XS * EDSCALE);
	item_style->set_content_margin(SIDE_BOTTOM, AIColors::PADDING_XS * EDSCALE);
	item_panel->add_theme_style_override("panel", item_style);

	// HBox for content and buttons
	HBoxContainer *item_hbox = memnew(HBoxContainer);
	item_hbox->add_theme_constant_override("separation", AIColors::PADDING_SM * EDSCALE);
	item_panel->add_child(item_hbox);

	// VBox for message text and subtitle
	VBoxContainer *text_vbox = memnew(VBoxContainer);
	text_vbox->set_h_size_flags(SIZE_EXPAND_FILL);
	text_vbox->add_theme_constant_override("separation", 0);
	item_hbox->add_child(text_vbox);

	// Message text (truncated)
	Label *msg_label = memnew(Label);
	String display_text = p_msg.text;
	// Truncate to ~50 chars and add ellipsis
	if (display_text.length() > 50) {
		display_text = display_text.substr(0, 47) + "...";
	}
	// Replace newlines with spaces for single-line display
	display_text = display_text.replace("\n", " ").replace("\r", "");
	msg_label->set_text(display_text);
	msg_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	msg_label->add_theme_color_override("font_color", AIColors::TEXT_PRIMARY);
	text_vbox->add_child(msg_label);

	// Subtitle
	Label *subtitle_label = memnew(Label);
	subtitle_label->set_text(TTR("Will be read at the AI's next step"));
	subtitle_label->add_theme_color_override("font_color", AIColors::TEXT_MUTED);
	subtitle_label->add_theme_font_size_override("font_size", 11 * EDSCALE);
	text_vbox->add_child(subtitle_label);

	// Edit button (pencil icon or "Edit" text)
	Button *edit_btn = memnew(Button);
	edit_btn->set_flat(true);
	edit_btn->set_text(TTR("Edit"));
	edit_btn->add_theme_color_override("font_color", AIColors::TEXT_SECONDARY);
	edit_btn->add_theme_color_override("font_hover_color", AIColors::ACCENT_BLUE);
	edit_btn->connect(SceneStringNames::get_singleton()->pressed, callable_mp(this, &AIStatusPanel::_on_queue_item_edit).bind(p_index));
	item_hbox->add_child(edit_btn);

	// Remove button (X)
	Button *remove_btn = memnew(Button);
	remove_btn->set_flat(true);
	remove_btn->set_text(String::utf8("✕"));
	remove_btn->add_theme_color_override("font_color", AIColors::TEXT_MUTED);
	remove_btn->add_theme_color_override("font_hover_color", AIColors::ERROR);
	remove_btn->connect(SceneStringNames::get_singleton()->pressed, callable_mp(this, &AIStatusPanel::_on_queue_item_remove).bind(p_index));
	item_hbox->add_child(remove_btn);

	return item_panel;
}

void AIStatusPanel::_on_queue_item_edit(int p_index) {
	if (p_index < 0 || p_index >= message_queue.size()) {
		return;
	}

	// Get the message text
	String text = message_queue[p_index].text;

	// Remove from queue (and withdraw it from the live run's pending injections)
	_withdraw_pending_injection(message_queue[p_index].id);
	message_queue.remove_at(p_index);
	_update_queue_ui();

	// Put the text in the input field for editing
	if (prompt_edit) {
		prompt_edit->set_text(text);
		prompt_edit->grab_focus();
		// Move cursor to end
		prompt_edit->set_caret_line(prompt_edit->get_line_count() - 1);
		prompt_edit->set_caret_column(prompt_edit->get_line(prompt_edit->get_line_count() - 1).length());
	}

	_update_send_button_state();
	print_line(vformat("AI Queue: Edited message at index %d, %d remaining", p_index, message_queue.size()));
}

void AIStatusPanel::_on_queue_item_remove(int p_index) {
	_remove_queued_message(p_index);
}

// ============================================================================
// Message Queue Management
// ============================================================================

String AIStatusPanel::_generate_queue_id() {
	static uint64_t counter = 0;
	return vformat("queue_%d_%d", OS::get_singleton()->get_ticks_msec(), counter++);
}

String AIStatusPanel::_enqueue_message(const String &p_text) {
	QueuedMessage msg;
	msg.id = _generate_queue_id();
	msg.text = p_text;
	msg.created_at = OS::get_singleton()->get_ticks_msec();
	message_queue.push_back(msg);

	print_line(vformat("AI Queue: Enqueued message (queue size: %d)", message_queue.size()));
	_update_queue_ui();
	return msg.id;
}

void AIStatusPanel::_dequeue_and_run_next() {
	if (message_queue.is_empty()) {
		return;
	}

	if (run_state != STATE_IDLE) {
		// Still running, don't dequeue
		return;
	}

	QueuedMessage next = message_queue[0];
	message_queue.remove_at(0);

	print_line(vformat("AI Queue: Dequeued message, %d remaining", message_queue.size()));
	_update_queue_ui();

	// Start the run with the dequeued message
	_start_run(next.text);
}

void AIStatusPanel::_remove_queued_message(int p_index) {
	if (p_index < 0 || p_index >= message_queue.size()) {
		return;
	}

	_withdraw_pending_injection(message_queue[p_index].id);
	message_queue.remove_at(p_index);
	print_line(vformat("AI Queue: Removed message at index %d, %d remaining", p_index, message_queue.size()));
	_update_queue_ui();
}

void AIStatusPanel::_withdraw_pending_injection(const String &p_queue_id) {
	if (!Engine::get_singleton()->has_singleton("AI")) {
		return;
	}
	AI *ai = Object::cast_to<AI>(Engine::get_singleton()->get_singleton_object("AI"));
	if (ai) {
		Ref<AgenticOrchestrator> orchestrator = ai->get_orchestrator();
		if (orchestrator.is_valid()) {
			orchestrator->remove_pending_injection(p_queue_id);
		}
	}
}

void AIStatusPanel::_on_user_injection_consumed(const Array &p_ids) {
	// The orchestrator just appended these queued messages to the model
	// conversation (all of this batch's tool results are already persisted,
	// so appending here keeps store order identical to wire order).
	for (int i = 0; i < p_ids.size(); i++) {
		String id = p_ids[i];
		for (int j = 0; j < message_queue.size(); j++) {
			if (message_queue[j].id != id) {
				continue;
			}
			if (chat_store.is_valid()) {
				HistoryItem user_item = chat_store->append_item(AIChatStore::make_user_item(message_queue[j].text, Vector<String>()));
				_append_message_ui(user_item);
				should_auto_scroll = true;
			}
			message_queue.remove_at(j);
			break;
		}
	}
	_update_queue_ui();
}

// ============================================================================
// Run State Management
// ============================================================================

void AIStatusPanel::_set_run_state(RunState p_state) {
	if (run_state == p_state) {
		return;
	}

	run_state = p_state;
	print_line(vformat("AI Run State: %s", p_state == STATE_IDLE ? "IDLE" : "RUNNING"));
	_update_send_button_state();
}

void AIStatusPanel::_start_run(const String &p_message) {
	if (run_state != STATE_IDLE) {
		// Already running - this shouldn't happen, but enqueue just in case
		_enqueue_message(p_message);
		return;
	}

	_run_token_total = 0;
	_last_turn_tokens = 0;

	// Tell AI singleton which conversation this run belongs to (for per-chat journal files)
	AI *ai = AI::get_singleton();
	if (ai && chat_store.is_valid()) {
		ai->set_current_chat_id(chat_store->get_chat_id());
	}

	// Snapshot and consume pending images before async work begins
	Vector<String> images_for_run = pending_images;
	_clear_pending_images();

	// Add debug context bubble above user message (if pill is active)
	Control *debug_bubble = _create_debug_context_bubble();
	if (debug_bubble && message_list) {
		message_list->add_child(debug_bubble);
	}

	// Add parse error bubble above user message (if pill is active)
	Control *parse_bubble = _create_parse_error_bubble();
	if (parse_bubble && message_list) {
		message_list->add_child(parse_bubble);
	}

	// Persist parse_error_state to chat store for external dashboards
	if (chat_store.is_valid() && parse_error_pill && parse_error_pill->has_content()) {
		chat_store->append_item(AIChatStore::make_parse_error_state_item(
				parse_error_pill->get_file_path(), parse_error_pill->get_errors()));
	}

	// Persist engine_state to chat store so external dashboards can display included errors
	if (chat_store.is_valid() && debug_pill && debug_pill->has_content()) {
		bool game_running = EditorRunBar::get_singleton() ? EditorRunBar::get_singleton()->is_playing() : false;
		EditorDebuggerNode *edn = EditorDebuggerNode::get_singleton();
		int err_count = 0;
		int warn_count = 0;
		Array errors;
		if (edn) {
			ScriptEditorDebugger *dbg = edn->get_default_debugger();
			if (dbg) {
				err_count = dbg->get_error_count();
				warn_count = dbg->get_warning_count();
				errors = dbg->get_structured_errors(20, 4);
			}
		}
		chat_store->append_item(AIChatStore::make_engine_state_item(
				game_running, err_count, warn_count, errors));
	}

	// Append user message to store
	current_run_user_message_id = 0;
	if (chat_store.is_valid()) {
		HistoryItem user_item = chat_store->append_item(AIChatStore::make_user_item(p_message, images_for_run));
		current_run_user_message_id = user_item.ts; // ts is used as anchor for checkpoints
		_append_message_ui(user_item);
	}

	// Persist which model is handling this turn
	if (chat_store.is_valid()) {
		if (use_harness_mode) {
			chat_store->append_item(AIChatStore::make_model_info_item(harness_model, "codex-harness"));
		} else {
			AI *ai_pre = AI::get_singleton();
			if (ai_pre) {
				Ref<AIProvider> prov = ai_pre->get_provider();
				if (prov.is_valid()) {
					chat_store->append_item(AIChatStore::make_model_info_item(
							prov->get_model(), prov->get_provider_name()));
				}
			}
		}
	}

	// Show pending message
	_show_pending_message();

	// Update state
	_set_run_state(STATE_RUNNING);
	is_waiting_for_response = true;

	if (use_harness_mode) {
		// Codex harness loop: the child process owns history and context;
		// only the new user text is sent (no _build_model_messages).
		_ensure_harness_driver();
		if (harness_driver.is_valid() && harness_driver->is_session_ready()) {
			print_line("AI Chat Panel: Starting codex harness run");
			harness_driver->send_user_message(p_message, current_run_user_message_id);
		} else if (harness_driver.is_valid()) {
			// Session still coming up: the driver queues the message itself.
			print_line("AI Chat Panel: Harness session starting; message queued");
			harness_driver->send_user_message(p_message, current_run_user_message_id);
		} else {
			ERR_PRINT("AI Chat Panel: Codex harness driver unavailable.");
			_remove_pending_message();
			is_waiting_for_response = false;
			_set_run_state(STATE_IDLE);
		}
		return;
	}

	// Build full message history for context
	Array messages = _build_model_messages();

	// Send to AI via agentic orchestrator
	if (Engine::get_singleton()->has_singleton("AI")) {
		Object *ai_obj = Engine::get_singleton()->get_singleton_object("AI");
		AI *ai = Object::cast_to<AI>(ai_obj);
		if (ai) {
			Ref<AIProvider> provider = ai->get_provider();
			Ref<AgenticOrchestrator> orchestrator = ai->get_orchestrator();

			if (orchestrator.is_valid() && provider.is_valid()) {
				print_line(vformat("AI Chat Panel: Starting agentic run with %d messages", messages.size()));
				orchestrator->run_agentic_loop(messages, provider);
				// Set user message ID for checkpoint anchoring
				orchestrator->set_user_message_id(current_run_user_message_id);
			} else {
				ERR_PRINT("AI Chat Panel: Orchestrator or provider not available.");
				_remove_pending_message();
				is_waiting_for_response = false;
				_set_run_state(STATE_IDLE);
			}
		}
	} else {
		ERR_PRINT("AI Chat Panel: AI singleton not found.");
		_remove_pending_message();
		is_waiting_for_response = false;
		_set_run_state(STATE_IDLE);
	}
}

void AIStatusPanel::_request_cancel() {
	if (run_state != STATE_RUNNING) {
		return;
	}

	// cancel_run is terminal and instant: it repairs the transcript and emits
	// run_complete synchronously, which lands in _on_orchestrator_complete and
	// resets run_state to IDLE before this returns.
	if (use_harness_mode) {
		// Harness: turn/interrupt round-trips through codex; run_complete
		// arrives asynchronously (~10ms measured), after the IDLE fallback.
		if (harness_driver.is_valid() && harness_driver->is_running()) {
			print_line("AI Chat Panel: Interrupting codex harness run");
			harness_driver->cancel_run();
		}
	} else if (Engine::get_singleton()->has_singleton("AI")) {
		Object *ai_obj = Engine::get_singleton()->get_singleton_object("AI");
		AI *ai = Object::cast_to<AI>(ai_obj);
		if (ai) {
			Ref<AgenticOrchestrator> orchestrator = ai->get_orchestrator();
			if (orchestrator.is_valid() && orchestrator->is_running()) {
				print_line("AI Chat Panel: Cancelling agentic run");
				orchestrator->cancel_run();
			}
		}
	}

	// Desync fallback: if the orchestrator wasn't actually running (so no
	// run_complete fired), don't leave the composer stuck in RUNNING.
	if (run_state == STATE_RUNNING) {
		_set_run_state(STATE_IDLE);
	}
}

void AIStatusPanel::_add_pending_image(Ref<Image> p_image) {
	// Encode (resizes to 512px max)
	String b64 = _encode_image_for_api(p_image);
	if (b64.is_empty()) {
		WARN_PRINT("AI: Failed to encode clipboard image.");
		return;
	}
	pending_images.push_back(b64);
	pending_images_raw.push_back(p_image);
	_rebuild_image_preview_strip();
	_update_send_button_state();
}

void AIStatusPanel::_remove_pending_image(int p_index) {
	if (p_index < 0 || p_index >= pending_images.size()) {
		return;
	}
	pending_images.remove_at(p_index);
	pending_images_raw.remove_at(p_index);
	_rebuild_image_preview_strip();
	_update_send_button_state();
}

void AIStatusPanel::_clear_pending_images() {
	pending_images.clear();
	pending_images_raw.clear();
	_rebuild_image_preview_strip();
}

void AIStatusPanel::_rebuild_image_preview_strip() {
	if (!image_preview_strip) {
		return;
	}
	// Remove all existing thumbnails
	while (image_preview_strip->get_child_count() > 0) {
		Node *child = image_preview_strip->get_child(0);
		image_preview_strip->remove_child(child);
		child->queue_free();
	}
	// Rebuild from pending_images_raw
	for (int i = 0; i < pending_images_raw.size(); i++) {
		PanelContainer *thumb_panel = memnew(PanelContainer);
		Ref<StyleBoxFlat> thumb_style;
		thumb_style.instantiate();
		thumb_style->set_bg_color(AIColors::BG_3);
		thumb_style->set_corner_radius_all(AIColors::CORNER_RADIUS_SM * EDSCALE);
		thumb_style->set_content_margin_all(2 * EDSCALE);
		thumb_panel->add_theme_style_override("panel", thumb_style);

		VBoxContainer *inner = memnew(VBoxContainer);
		thumb_panel->add_child(inner);

		TextureRect *tex = memnew(TextureRect);
		Ref<ImageTexture> img_tex = ImageTexture::create_from_image(pending_images_raw[i]);
		tex->set_texture(img_tex);
		tex->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_COVERED);
		tex->set_custom_minimum_size(Size2(64, 64) * EDSCALE);
		inner->add_child(tex);

		Button *remove_btn = memnew(Button);
		remove_btn->set_text("x");
		remove_btn->set_flat(true);
		remove_btn->add_theme_color_override("font_color", AIColors::TEXT_MUTED);
		remove_btn->connect(SceneStringNames::get_singleton()->pressed,
				callable_mp(this, &AIStatusPanel::_remove_pending_image).bind(i));
		inner->add_child(remove_btn);

		image_preview_strip->add_child(thumb_panel);
	}
	image_preview_strip->set_visible(!pending_images_raw.is_empty());
}

void AIStatusPanel::_show_image_popup(const PackedStringArray &p_b64s) {
	if (!image_viewer || p_b64s.is_empty()) {
		return;
	}
	// Open on the *last* image — that's the one shown topmost in the stack
	// thumbnail, so the user expects the click to land there. Single-image
	// callers see no behaviour change (size-1 case has only index 0).
	image_viewer->popup_for_images(p_b64s, p_b64s.size() - 1);
}

void AIStatusPanel::_on_thumbnail_gui_input(const Ref<InputEvent> &p_event, const String &p_base64) {
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MouseButton::LEFT) {
		PackedStringArray single;
		single.push_back(p_base64);
		_show_image_popup(single);
		get_viewport()->set_input_as_handled();
	}
}

// Returns an estimate of content size in chars for a HistoryItem (for context budget).
static int _item_char_count(const HistoryItem &p_item) {
	String role = p_item.role();
	if (role == "user") {
		int chars = String(p_item.data.get("content", "")).length();
		Array images = p_item.data.get("images", Array());
		for (int j = 0; j < images.size(); j++) {
			chars += String(images[j]).length();
		}
		return chars;
	} else if (role == "assistant") {
		int chars = 0;
		Array blocks = p_item.data.get("content", Array());
		for (int j = 0; j < blocks.size(); j++) {
			Dictionary block = blocks[j];
			chars += String(block.get("text", "")).length();
			chars += JSON::stringify(block.get("args", Dictionary())).length();
		}
		return chars;
	} else if (role == "tool") {
		return JSON::stringify(p_item.data.get("content", Dictionary())).length();
	}
	return 0;
}

Array AIStatusPanel::_build_model_messages() {
	Array messages;
	context_was_truncated = false;

	if (!chat_store.is_valid()) {
		return messages;
	}

	const Vector<HistoryItem> &items = chat_store->get_items();
	if (items.is_empty()) {
		return messages;
	}

	// Determine char budget from the active model's context window.
	// Reserve ~10k tokens (40k chars) for system prompt + model output.
	int max_context_chars = DEFAULT_MAX_CONTEXT_CHARS;
	if (Engine::get_singleton()->has_singleton("AI")) {
		Object *ai_obj = Engine::get_singleton()->get_singleton_object("AI");
		AI *ai_inst = Object::cast_to<AI>(ai_obj);
		if (ai_inst && ai_inst->get_provider().is_valid()) {
			int window_tokens = ai_inst->get_provider()->get_context_window_tokens();
			if (window_tokens > 0) {
				max_context_chars = (window_tokens - 10000) * 4;
			}
		}
	}

	// Calculate total chars and find truncation point (backwards from latest).
	int total_chars = 0;
	int start_index = 0;
	for (int i = items.size() - 1; i >= 0; i--) {
		if (items[i].is_injection()) {
			continue; // injections don't count toward context budget
		}
		int item_chars = _item_char_count(items[i]);
		total_chars += item_chars;
		if (total_chars > max_context_chars) {
			start_index = i + 1;
			context_was_truncated = true;
			WARN_PRINT(vformat("AI: Context truncated. Using items from index %d of %d (%d chars, budget %d).",
				start_index, items.size(), total_chars - item_chars, max_context_chars));
			break;
		}
	}

	// Build OpenAI wire format messages from HistoryItems.
	for (int i = start_index; i < items.size(); i++) {
		const HistoryItem &item = items[i];

		if (item.is_injection()) {
			continue; // injections handled by orchestrator at runtime
		}

		String role = item.role();
		Dictionary msg;

		if (role == "user") {
			msg["role"] = "user";
			String content = item.data.get("content", "");
			// Wrap to prevent prompt injection; add timestamp prefix for session awareness
			String prefix;
			if (item.ts > 0) {
				Dictionary dt = Time::get_singleton()->get_datetime_dict_from_unix_time(item.ts / 1000);
				prefix = vformat("[%04d-%02d-%02d %02d:%02d] ", (int)dt["year"], (int)dt["month"], (int)dt["day"], (int)dt["hour"], (int)dt["minute"]);
			}
			msg["content"] = vformat("<user_message>\n%s%s\n</user_message>", prefix, content);
			// Attach images for provider formatting
			Array images = item.data.get("images", Array());
			if (!images.is_empty()) {
				msg["_images"] = images;
			}

		} else if (role == "assistant") {
			// Convert canonical content_blocks to OpenAI wire format
			Array content_blocks = item.data.get("content", Array());
			String text_content;
			Array tool_calls_wire;
			for (int j = 0; j < content_blocks.size(); j++) {
				Dictionary block = content_blocks[j];
				String type = block.get("type", "");
				if (type == "text") {
					if (!text_content.is_empty()) {
						text_content += "\n";
					}
					text_content += String(block.get("text", ""));
				} else if (type == "tool_call") {
					Dictionary tc;
					tc["id"] = block.get("id", "");
					tc["type"] = "function";
					Dictionary func;
					func["name"] = block.get("name", "");
					func["arguments"] = JSON::stringify(block.get("args", Dictionary()));
					tc["function"] = func;
					tool_calls_wire.push_back(tc);
				}
			}
			// Skip items with no text and no tool calls — strict providers
			// (Moonshot) reject empty assistant messages with HTTP 400.
			// Transcripts saved before the orchestrator stopped persisting
			// them can still contain one (e.g. a reasoning model that burned
			// its whole completion budget thinking). Skipping may leave
			// consecutive user messages, which AnthropicProvider already
			// merges for the one API that enforces alternation.
			if (text_content.is_empty() && tool_calls_wire.is_empty()) {
				print_line("AI: Skipped empty assistant message during history rebuild.");
				continue;
			}
			msg["role"] = "assistant";
			msg["content"] = text_content.is_empty() ? Variant() : Variant(text_content);
			if (!tool_calls_wire.is_empty()) {
				msg["tool_calls"] = tool_calls_wire;
			}

		} else if (role == "tool") {
			msg["role"] = "tool";
			msg["tool_call_id"] = item.data.get("tool_call_id", "");
			Dictionary tool_content = item.data.get("content", Dictionary());
			// Content is a Dictionary in canonical format; serialize to string for OpenAI
			msg["content"] = JSON::stringify(tool_content);

			// Reload persisted screenshot(s) for vision (cross-session continuity).
			// Walks both the single-shot (`screenshot`) and multi-shot
			// (`screenshots[].screenshot`) shapes — without the multi-shot
			// branch, reloading a chat that ran a multi-screenshot turn would
			// only resend the first image to the model on the next request.
			if (tool_content.has("result") && chat_store.is_valid()) {
				Dictionary result = tool_content["result"];
				Array imgs;
				if (result.has("screenshot")) {
					String b64 = chat_store->load_screenshot_b64(result["screenshot"]);
					if (!b64.is_empty()) {
						imgs.push_back(b64);
					}
				}
				if (result.has("screenshots") && result["screenshots"].get_type() == Variant::ARRAY) {
					Array shots = result["screenshots"];
					for (int si = 0; si < shots.size(); si++) {
						Dictionary entry = shots[si];
						if (entry.has("screenshot")) {
							String b64 = chat_store->load_screenshot_b64(entry["screenshot"]);
							if (!b64.is_empty()) {
								imgs.push_back(b64);
							}
						}
					}
				}
				if (!imgs.is_empty()) {
					msg["_images"] = imgs;
				}
			}
		}

		if (!msg.is_empty()) {
			messages.push_back(msg);
		}
	}

	// Repair pass: every assistant tool_call must be answered by a tool message
	// before the next non-tool message, or strict OpenAI-compatible providers
	// reject the whole request (HTTP 400). Transcripts saved before update_todos
	// results were persisted have dangling calls, and a crash mid-run can leave
	// them too — synthesize a stub response for each so those chats stay usable.
	for (int i = 0; i < messages.size(); i++) {
		Dictionary msg = messages[i];
		if (String(msg.get("role", "")) != "assistant" || !msg.has("tool_calls")) {
			continue;
		}
		int next_non_tool = i + 1;
		Vector<String> answered_ids;
		while (next_non_tool < messages.size()) {
			Dictionary next = messages[next_non_tool];
			if (String(next.get("role", "")) != "tool") {
				break;
			}
			answered_ids.push_back(next.get("tool_call_id", ""));
			next_non_tool++;
		}
		Array tool_calls = msg["tool_calls"];
		for (int j = 0; j < tool_calls.size(); j++) {
			Dictionary tc = tool_calls[j];
			String call_id = tc.get("id", "");
			if (call_id.is_empty() || answered_ids.has(call_id)) {
				continue;
			}
			Dictionary func = tc.get("function", Dictionary());
			Dictionary stub_content;
			stub_content["status"] = "unknown";
			stub_content["tool_name"] = func.get("name", "");
			stub_content["note"] = "Tool result was not recorded in the transcript.";
			Dictionary stub;
			stub["role"] = "tool";
			stub["tool_call_id"] = call_id;
			stub["content"] = JSON::stringify(stub_content);
			messages.insert(next_non_tool, stub);
			next_non_tool++;
			print_line(vformat("AI: Synthesized stub tool response for dangling tool_call '%s' (%s).",
					call_id, String(func.get("name", ""))));
		}
		i = next_non_tool - 1;
	}

	// Log context info
	int final_chars = 0;
	for (int i = start_index; i < items.size(); i++) {
		if (!items[i].is_injection()) {
			final_chars += _item_char_count(items[i]);
		}
	}
	print_line(vformat("AI: Built %d messages for context (%d chars)%s",
		messages.size(), final_chars, context_was_truncated ? " [TRUNCATED]" : ""));

	_update_context_usage(final_chars, max_context_chars);

	return messages;
}

void AIStatusPanel::_on_send_button_pressed() {
	// Button behavior depends on current run state
	switch (run_state) {
		case STATE_IDLE: {
			// Send mode: try to send the message
			if (!prompt_edit || context_exhausted) {
				return;
			}

			String prompt_text = prompt_edit->get_text().strip_edges();
			if (prompt_text.is_empty() && pending_images.is_empty()) {
				return;
			}

			// If we're in pending edit send mode, show dialog first (unless "don't ask again")
			if (is_pending_edit_send) {
				if (skip_edit_send_dialog) {
					// User chose "don't ask again" - just send without reverting
					_on_dialog_continue_no_revert();
					return;
				}

				// Update dialog for edit send confirmation
				if (rewind_dialog_label) {
					rewind_dialog_label->set_text(TTR("You can optionally revert project changes (undo editor actions) made by the AI after this message."));
				}
				if (rewind_dialog) {
					rewind_dialog->set_title(TTR("Submit edited message?"));
				}

				// Update revert button availability
				if (continue_revert_button) {
					continue_revert_button->set_disabled(!undo_available_for_edit);
					if (!undo_available_for_edit) {
						continue_revert_button->set_tooltip_text(TTR("Project revert not available (no prior checkpoint)"));
					} else {
						continue_revert_button->set_tooltip_text("");
					}
				}

				// Show dialog
				if (rewind_dialog) {
					rewind_dialog->reset_size();
					rewind_dialog->popup_centered();
				}
				return;
			}

			// Normal send: clear input and start run
			prompt_edit->set_text("");
			_update_send_button_state();

			// Start the run
			_start_run(prompt_text);
		} break;

		case STATE_RUNNING: {
			// Stop mode: request cancellation
			_request_cancel();
		} break;
	}
}

// ============================================================
// Multi-chat management
// ============================================================

void AIStatusPanel::_on_turn_tokens_ready(int p_tokens) {
	_last_turn_tokens = p_tokens;
}

void AIStatusPanel::_insert_token_total_label_into(VBoxContainer *p_list, int p_total, Control *p_before) {
	if (p_total <= 0 || !p_list) {
		return;
	}
	String text;
	if (p_total >= 10000) {
		text = vformat("Total: %dk", p_total / 1000);
	} else {
		text = vformat("Total: %d", p_total);
	}
	Label *lbl = memnew(Label);
	lbl->set_text(text);
	lbl->set_h_size_flags(SIZE_EXPAND_FILL);
	lbl->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
	lbl->add_theme_color_override("font_color", AIColors::TEXT_MUTED);
	p_list->add_child(lbl);
	if (p_before) {
		p_list->move_child(lbl, p_before->get_index());
	}
}

void AIStatusPanel::_insert_token_total_label() {
	// Prefer the actual API-reported total_tokens; fall back to our per-tool estimates.
	int total = _last_turn_tokens > 0 ? _last_turn_tokens : _run_token_total;
	_insert_token_total_label_into(message_list, total, pending_message);
}

static void _toggle_latency_labels_recursive(Node *p_node, bool p_visible) {
	Label *lbl = Object::cast_to<Label>(p_node);
	if (lbl && lbl->has_meta("_ai_latency_label")) {
		lbl->set_visible(p_visible);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_toggle_latency_labels_recursive(p_node->get_child(i), p_visible);
	}
}

void AIStatusPanel::_on_token_toggle_pressed() {
	_show_token_counts = !_show_token_counts;
	// Walk message_list children and toggle token labels on all ToolCollapsibleEntry nodes,
	// plus latency footers on assistant bubbles.
	for (int i = 0; i < message_list->get_child_count(); i++) {
		Node *child = message_list->get_child(i);
		ToolCollapsibleEntry *entry = Object::cast_to<ToolCollapsibleEntry>(child);
		if (entry) {
			entry->set_token_label_visible(_show_token_counts);
		} else {
			_toggle_latency_labels_recursive(child, _show_token_counts);
		}
	}
}

void AIStatusPanel::_new_chat() {
	if (run_state != STATE_IDLE) {
		return; // Don't switch while running
	}
	_cancel_pending_edit();
	_clear_pending_images();
	message_queue.clear();
	_update_queue_ui();
	context_exhausted = false;

	// Harness: a new chat must get its own codex thread. Without this reset
	// the new chat silently shares the previous chat's thread — and both
	// chats' histories bleed together (observed 7/28).
	if (harness_driver.is_valid()) {
		harness_driver->shutdown();
		harness_driver.unref();
	}
	harness_streaming = false;
	harness_stream_text = String();
	harness_stream_block = nullptr;
	harness_stream_rich = nullptr;
	_finalize_harness_thinking();
	approval_queue.clear();
	_show_next_approval(); // Restores the composer if an approval was showing.

	if (chat_store.is_valid()) {
		String new_id = AIChatStore::generate_chat_id();
		chat_store->set_file_path(AIChatStore::make_chat_path(new_id));
	}
	_rebuild_message_list();
	_reset_context_usage();
	_update_send_button_state();
}

String AIStatusPanel::_get_chat_display_name(const String &p_id) const {
	String path = AIChatStore::make_chat_path(p_id);
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
	if (f.is_null()) {
		return p_id;
	}
	// Read JSONL lines until we find the first user message.
	while (!f->eof_reached()) {
		String line = f->get_line().strip_edges();
		if (line.is_empty()) {
			continue;
		}
		Variant parsed = JSON::parse_string(line);
		if (parsed.get_type() != Variant::DICTIONARY) {
			continue;
		}
		Dictionary d = parsed;
		// JSONL format: {"ts": ..., "item": {"role":"user", "content":"..."}}
		Dictionary item = d.get("item", Dictionary());
		if (item.get("role", "") != "user") {
			continue;
		}
		String text = item.get("content", "");
		text = text.replace("\n", " ").replace("\t", " ").strip_edges();
		return text.is_empty() ? p_id : text;
	}
	return p_id;
}

String AIStatusPanel::_format_relative_time(uint64_t p_unix_time) {
	uint64_t now = Time::get_singleton()->get_unix_time_from_system();
	if (p_unix_time > now) {
		return "now";
	}
	uint64_t diff = now - p_unix_time;
	if (diff < 60) {
		return "now";
	} else if (diff < 3600) {
		return vformat("%dm", diff / 60);
	} else if (diff < 86400) {
		return vformat("%dh", diff / 3600);
	} else if (diff < 86400 * 30) {
		return vformat("%dd", diff / 86400);
	} else if (diff < 86400 * 365) {
		return vformat("%dmo", diff / (86400 * 30));
	} else {
		return vformat("%dy", diff / (86400 * 365));
	}
}

void AIStatusPanel::_switch_to_chat(const String &p_id) {
	if (run_state != STATE_IDLE) {
		return;
	}
	if (chat_store.is_valid() && chat_store->get_chat_id() == p_id) {
		history_popup->hide();
		return;
	}
	_cancel_pending_edit();
	_clear_pending_images();
	message_queue.clear();
	_update_queue_ui();
	context_exhausted = false;

	// Harness: each chat maps to its own codex thread; drop the driver so the
	// next run brings up (or resumes) the selected chat's thread.
	if (harness_driver.is_valid()) {
		harness_driver->shutdown();
		harness_driver.unref();
	}
	harness_streaming = false;
	harness_stream_text = String();
	harness_stream_block = nullptr;
	harness_stream_rich = nullptr;
	_finalize_harness_thinking();
	approval_queue.clear();
	_show_next_approval(); // Restores the composer if an approval was showing.

	if (chat_store.is_valid()) {
		chat_store->set_file_path(AIChatStore::make_chat_path(p_id));
		chat_store->load_items();
	}
	_rebuild_message_list();
	_refresh_context_usage();
	_update_send_button_state();
	history_popup->hide();
}

void AIStatusPanel::_delete_chat(const String &p_id) {
	pending_delete_chat_id = p_id;
	if (delete_chat_dialog) {
		delete_chat_dialog->popup_centered();
	}
}

void AIStatusPanel::_on_delete_chat_confirmed() {
	if (pending_delete_chat_id.is_empty()) {
		return;
	}
	String path = AIChatStore::make_chat_path(pending_delete_chat_id);
	bool is_current = chat_store.is_valid() && chat_store->get_chat_id() == pending_delete_chat_id;

	// Delete the file
	if (FileAccess::exists(path)) {
		String dir = path.get_base_dir();
		String filename = path.get_file();
		Ref<DirAccess> da = DirAccess::open(dir);
		if (da.is_valid()) {
			da->remove(filename);
		}
	}

	pending_delete_chat_id = "";
	history_popup->hide();

	if (is_current) {
		// Switch to another chat or create new
		Vector<String> ids = AIChatStore::list_chat_ids();
		if (!ids.is_empty()) {
			_switch_to_chat(ids[0]);
		} else {
			_new_chat();
		}
	}
}

void AIStatusPanel::_rebuild_history_popup() {
	if (!history_list) {
		return;
	}
	// Clear existing items
	while (history_list->get_child_count() > 0) {
		Node *child = history_list->get_child(0);
		history_list->remove_child(child);
		child->queue_free();
	}

	Vector<String> ids = AIChatStore::list_chat_ids();
	String current_id = chat_store.is_valid() ? chat_store->get_chat_id() : "";

	if (ids.is_empty()) {
		Label *empty_label = memnew(Label);
		empty_label->set_text(TTR("No saved chats"));
		empty_label->add_theme_color_override("font_color", AIColors::TEXT_MUTED);
		history_list->add_child(empty_label);
		return;
	}

	for (int i = 0; i < ids.size(); i++) {
		const String &id = ids[i];
		String display = _get_chat_display_name(id);
		bool is_current = (id == current_id);

		HBoxContainer *row = memnew(HBoxContainer);
		row->add_theme_constant_override("separation", 4 * EDSCALE);
		history_list->add_child(row);

		// Chat name button (click to switch)
		Button *name_btn = memnew(Button);
		name_btn->set_text(display);
		name_btn->set_h_size_flags(SIZE_EXPAND_FILL);
		name_btn->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
		name_btn->set_clip_text(true);
		name_btn->set_default_cursor_shape(Control::CURSOR_POINTING_HAND);

		// Transparent normal state (replaces flat=true so hover still draws)
		Ref<StyleBoxFlat> name_normal;
		name_normal.instantiate();
		name_normal->set_bg_color(Color(0, 0, 0, 0));
		name_normal->set_content_margin_all(AIColors::PADDING_XS * EDSCALE);
		name_btn->add_theme_style_override("normal", name_normal);
		name_btn->add_theme_style_override("pressed", name_normal);
		name_btn->add_theme_style_override("focus", name_normal);

		Ref<StyleBoxFlat> name_hover;
		name_hover.instantiate();
		name_hover->set_corner_radius_all(AIColors::CORNER_RADIUS_SM * EDSCALE);
		name_hover->set_content_margin_all(AIColors::PADDING_XS * EDSCALE);
		if (is_current) {
			name_btn->add_theme_color_override("font_color", AIColors::ACCENT_BLUE);
			name_btn->add_theme_color_override("font_hover_color", AIColors::ACCENT_BLUE);
			name_hover->set_bg_color(AIColors::ACCENT_BLUE_MUTED);
		} else {
			name_btn->add_theme_color_override("font_color", AIColors::TEXT_PRIMARY);
			name_btn->add_theme_color_override("font_hover_color", AIColors::TEXT_PRIMARY);
			name_hover->set_bg_color(AIColors::BG_3);
		}
		name_btn->add_theme_style_override("hover", name_hover);

		name_btn->connect(SceneStringNames::get_singleton()->pressed, callable_mp(this, &AIStatusPanel::_switch_to_chat).bind(id));
		row->add_child(name_btn);

		// Time-since-last-activity label
		String chat_path = AIChatStore::make_chat_path(id);
		uint64_t mod_time = FileAccess::get_modified_time(chat_path);
		Label *time_label = memnew(Label);
		time_label->set_text(_format_relative_time(mod_time));
		time_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
		time_label->set_custom_minimum_size(Size2(36 * EDSCALE, 0));
		time_label->add_theme_color_override("font_color", AIColors::TEXT_MUTED);
		time_label->add_theme_font_size_override("font_size", 11 * EDSCALE);
		row->add_child(time_label);

		// Delete button
		Button *del_btn = memnew(Button);
		del_btn->set_text(TTR("x"));
		del_btn->set_default_cursor_shape(Control::CURSOR_POINTING_HAND);
		del_btn->add_theme_color_override("font_color", AIColors::TEXT_MUTED);
		del_btn->add_theme_color_override("font_hover_color", AIColors::TEXT_PRIMARY);
		del_btn->set_tooltip_text(TTR("Delete this chat"));

		Ref<StyleBoxFlat> del_normal;
		del_normal.instantiate();
		del_normal->set_bg_color(Color(0, 0, 0, 0));
		del_normal->set_content_margin_all(AIColors::PADDING_XS * EDSCALE);
		del_btn->add_theme_style_override("normal", del_normal);
		del_btn->add_theme_style_override("focus", del_normal);

		Ref<StyleBoxFlat> del_hover;
		del_hover.instantiate();
		del_hover->set_bg_color(AIColors::BG_3);
		del_hover->set_corner_radius_all(AIColors::CORNER_RADIUS_SM * EDSCALE);
		del_hover->set_content_margin_all(AIColors::PADDING_XS * EDSCALE);
		del_btn->add_theme_style_override("hover", del_hover);

		del_btn->connect(SceneStringNames::get_singleton()->pressed, callable_mp(this, &AIStatusPanel::_delete_chat).bind(id));
		row->add_child(del_btn);
	}
}

void AIStatusPanel::_show_history_popup() {
	_rebuild_history_popup();

	if (!history_popup) {
		return;
	}
	// Full width of the AI panel, positioned flush at its left edge just below the toolbar
	float panel_width = get_size().x;
	Vector2 panel_screen = get_screen_position();
	float toolbar_bottom = chat_toolbar ? (chat_toolbar->get_position().y + chat_toolbar->get_size().y) : 0.0f;

	history_popup->set_size(Size2(panel_width, 0)); // height: auto-fit to content
	history_popup->set_position(Vector2(panel_screen.x, panel_screen.y + toolbar_bottom));
	history_popup->popup();
}

void AIStatusPanel::_on_drag_handle_gui_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->get_button_index() == MouseButton::LEFT) {
		if (mb->is_pressed()) {
			_dragging_input = true;
			_drag_start_y = mb->get_global_position().y;
			_drag_start_height = prompt_edit->get_custom_minimum_size().y;
			input_drag_handle->accept_event();
		} else {
			_dragging_input = false;
		}
		return;
	}

	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid() && _dragging_input) {
		float delta = _drag_start_y - mm->get_global_position().y; // drag up = bigger
		float new_height = _drag_start_height + delta;

		float min_h = 60 * EDSCALE;
		float max_h = get_size().y * 0.5f;
		new_height = CLAMP(new_height, min_h, max_h);

		prompt_edit->set_custom_minimum_size(Size2(0, new_height));
		input_drag_handle->accept_event();
	}
}

void AIStatusPanel::_on_prompt_text_changed() {
	_update_send_button_state();
}

bool AIStatusPanel::_can_drop_data_fw(const Point2 &p_point, const Variant &p_data, Control *p_from) const {
	if (p_data.get_type() != Variant::DICTIONARY) {
		return false;
	}
	const Dictionary drag_data = p_data;
	if (!drag_data.has("type") || !drag_data.has("files")) {
		return false;
	}
	const String drag_type = drag_data["type"];
	if (drag_type != "files" && drag_type != "files_and_dirs") {
		return false;
	}
	const Vector<String> file_paths = drag_data["files"];
	return !file_paths.is_empty();
}

void AIStatusPanel::_drop_data_fw(const Point2 &p_point, const Variant &p_data, Control *p_from) {
	if (!prompt_edit || p_data.get_type() != Variant::DICTIONARY) {
		return;
	}
	const Dictionary drag_data = p_data;
	if (!drag_data.has("files")) {
		return;
	}
	const Vector<String> file_paths = drag_data["files"];
	if (file_paths.is_empty()) {
		return;
	}

	// Build the snippet: paths joined by spaces, with a trailing space so the
	// user can keep typing immediately after the drop without having to add one.
	String snippet;
	for (int i = 0; i < file_paths.size(); i++) {
		if (i > 0) {
			snippet += " ";
		}
		snippet += file_paths[i];
	}
	snippet += " ";

	prompt_edit->grab_focus();
	prompt_edit->insert_text_at_caret(snippet);
}

void AIStatusPanel::_on_prompt_gui_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventKey> key_event = p_event;
	if (key_event.is_valid() && key_event->is_pressed()) {
		// Escape: stop the current run
		if (key_event->get_keycode() == Key::ESCAPE) {
			_request_cancel();
			prompt_edit->accept_event();
			return;
		}

		// Shift+Tab: cycle approval policy (Ask -> Auto -> Read-only)
		if (key_event->get_keycode() == Key::TAB && key_event->is_shift_pressed()) {
			_cycle_policy_mode();
			prompt_edit->accept_event();
			return;
		}

		// Ctrl+V: intercept before TextEdit if clipboard has an image
		if (key_event->get_keycode() == Key::V && key_event->is_ctrl_pressed() &&
				!key_event->is_shift_pressed() && !key_event->is_alt_pressed()) {
			DisplayServer *ds = DisplayServer::get_singleton();
			if (ds && ds->clipboard_has_image()) {
				Ref<Image> img = ds->clipboard_get_image();
				if (img.is_valid() && !img->is_empty()) {
					_add_pending_image(img);
					prompt_edit->accept_event();
					return;
				}
			}
			// No image on clipboard — fall through to normal text paste
		}

		if (key_event->get_keycode() == Key::ENTER) {
			if (key_event->is_shift_pressed()) {
				// Shift+Enter: insert newline explicitly
				prompt_edit->accept_event();
				prompt_edit->insert_text_at_caret("\n");
			} else {
				// Enter without Shift: send the message
				// Skip if IME composition is active (best-effort for non-English input)
				if (prompt_edit->has_ime_text()) {
					return;
				}

				String prompt_text = prompt_edit->get_text().strip_edges();
				if (prompt_text.is_empty()) {
					return;
				}

				// Accept the event to prevent newline insertion
				prompt_edit->accept_event();

				// If running, queue the message and hand it to the orchestrator
				// as a mid-run injection: the model reads it at its next step
				// instead of after the whole run. Messages with attached images
				// stay queue-only (injections are text-only) so the images
				// travel with a fresh run instead of being silently dropped.
				if (run_state != STATE_IDLE) {
					if (use_harness_mode && pending_images.is_empty() && harness_driver.is_valid() && harness_driver->is_running()) {
						// Harness: steer the in-flight turn directly. The
						// message shows in the transcript as a user item.
						prompt_edit->set_text("");
						if (chat_store.is_valid()) {
							HistoryItem steer_item = chat_store->append_item(AIChatStore::make_user_item(prompt_text, Vector<String>()));
							_append_message_ui(steer_item);
						}
						harness_driver->steer(prompt_text);
						_update_send_button_state();
						prompt_edit->accept_event();
						return;
					}
					prompt_edit->set_text("");
					String queue_id = _enqueue_message(prompt_text);
					if (pending_images.is_empty() && Engine::get_singleton()->has_singleton("AI")) {
						AI *ai = Object::cast_to<AI>(Engine::get_singleton()->get_singleton_object("AI"));
						if (ai) {
							Ref<AgenticOrchestrator> orchestrator = ai->get_orchestrator();
							if (orchestrator.is_valid()) {
								// A false return means the run just ended; the
								// message stays queued and drains as a new run.
								orchestrator->inject_user_message(queue_id, prompt_text);
							}
						}
					}
					_update_send_button_state();
				} else {
					// Idle: trigger normal send
					_on_send_button_pressed();
				}
			}
		}
	}
}

void AIStatusPanel::_on_ai_response(bool p_success, const String &p_response, const String &p_error) {
	// If orchestrator is running, it handles responses via its own callbacks - skip this legacy handler
	if (Engine::get_singleton()->has_singleton("AI")) {
		Object *ai_obj = Engine::get_singleton()->get_singleton_object("AI");
		AI *ai = Object::cast_to<AI>(ai_obj);
		if (ai) {
			Ref<AgenticOrchestrator> orchestrator = ai->get_orchestrator();
			if (orchestrator.is_valid() && orchestrator->is_running()) {
				print_verbose("AIStatusPanel: Skipping legacy _on_ai_response - orchestrator is running");
				return;
			}
		}
	}

	// Remove pending message
	_remove_pending_message();

	// Append assistant response
	String content;
	if (p_success) {
		// Try to parse the new format: {"message": "...", "actions": [...]}
		JSON json;
		Error err = json.parse(p_response);
		if (err == OK && json.get_data().get_type() == Variant::DICTIONARY) {
			Dictionary response_dict = json.get_data();
			String message = response_dict.get("message", "");
			Array actions = response_dict.get("actions", Array());
			
			// Build display content
			if (!message.is_empty()) {
				content = message;
			}
			
			// If there are actions, append a summary
			if (actions.size() > 0) {
				if (!content.is_empty()) {
					content += "\n\n";
				}
				content += vformat("[%d action(s) executed]", actions.size());
			}
			
			// Fallback if both are empty
			if (content.is_empty()) {
				content = "(No response)";
			}
		} else {
			// Legacy format or parse error - show raw response
			content = p_response;
		}
	} else {
		content = vformat("[Error] %s", p_error);
	}

	if (chat_store.is_valid() && !content.is_empty()) {
		// Build minimal canonical assistant item for the legacy (non-agentic) path
		Array content_blocks;
		Dictionary text_block;
		text_block["type"] = "text";
		text_block["text"] = content;
		content_blocks.push_back(text_block);
		HistoryItem stored = chat_store->append_item(AIChatStore::make_assistant_item(content_blocks));
		_append_message_ui(stored);
	}

	// Update state
	is_waiting_for_response = false;
	_set_run_state(STATE_IDLE);
}

void AIStatusPanel::_on_orchestrator_started() {
	print_line("AIStatusPanel: Orchestrator run started");
	// State is already set to RUNNING by _start_run(), but this confirms orchestrator is active
	if (todo_panel) {
		todo_panel->set_visible(false);
	}
}

void AIStatusPanel::_on_api_round_started(int p_turn) {
	// Insert a visual divider before round 2+ so the user can see API call boundaries
	if (p_turn <= 1) {
		return;
	}

	_insert_token_total_label();
	_run_token_total = 0;
	_last_turn_tokens = 0;

	HSeparator *sep = memnew(HSeparator);
	sep->set_h_size_flags(SIZE_EXPAND_FILL);
	Ref<StyleBoxLine> style;
	style.instantiate();
	style->set_color(Color(1, 1, 1, 0.08f));
	style->set_thickness(1);
	sep->add_theme_style_override("separator", style);
	sep->add_theme_constant_override("separation", 6);

	if (pending_message) {
		message_list->add_child(sep);
		message_list->move_child(sep, pending_message->get_index());
	} else {
		message_list->add_child(sep);
	}
}

void AIStatusPanel::_on_todos_updated(const Array &p_todos) {
	if (!todo_panel) {
		return;
	}
	todo_panel->update_todos(p_todos);
	todo_panel->set_visible(!p_todos.is_empty());
}

void AIStatusPanel::_on_scene_diff_ready(const Dictionary &p_diff_info) {
	// Persist the injected scene diffs so the transcript and external
	// dashboards can show what the model was told. Not resent on reload
	// (injection items are skipped by _build_model_messages).
	if (!chat_store.is_valid()) {
		return;
	}
	chat_store->append_item(AIChatStore::make_scene_diff_item(
			p_diff_info.get("attribution", ""), p_diff_info.get("scenes", Array())));
}

void AIStatusPanel::_update_debug_pill() {
	if (!debug_pill) {
		return;
	}
#ifdef TOOLS_ENABLED
	bool game_running = false;
	int error_count = 0;
	int warning_count = 0;
	Array errors;

	EditorRunBar *run_bar = EditorRunBar::get_singleton();
	if (run_bar) {
		game_running = run_bar->is_playing();
	}
	EditorDebuggerNode *edn = EditorDebuggerNode::get_singleton();
	if (edn) {
		ScriptEditorDebugger *dbg = edn->get_default_debugger();
		if (dbg) {
			error_count = dbg->get_error_count();
			warning_count = dbg->get_warning_count();
			if ((error_count + warning_count) > 0) {
				errors = dbg->get_structured_errors(20, 4);
			}
		}
	}
	debug_pill->update_state(game_running, error_count, warning_count, errors);
#endif
}

void AIStatusPanel::_on_debug_pill_update_tick() {
	_update_debug_pill();
}

void AIStatusPanel::_on_debug_context_toggled(bool p_enabled) {
	// Forward toggle state to AI module so it skips context injection when disabled
	if (Engine::get_singleton()->has_singleton("AI")) {
		Object *ai_obj = Engine::get_singleton()->get_singleton_object("AI");
		AI *ai = Object::cast_to<AI>(ai_obj);
		if (ai) {
			ai->set_debug_context_enabled(p_enabled);
		}
	}
}

// Static callback for collapsible error bubbles in the chat transcript.
static void _on_error_bubble_gui_input(const Ref<InputEvent> &p_event, Object *p_bubble_obj) {
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->get_button_index() == MouseButton::LEFT && mb->is_pressed()) {
		Node *bubble = Object::cast_to<Node>(p_bubble_obj);
		if (!bubble) {
			return;
		}
		Variant hl_var = bubble->get_meta("_header_label", Variant());
		Variant el_var = bubble->get_meta("_error_list", Variant());
		RichTextLabel *header_label = Object::cast_to<RichTextLabel>(hl_var.operator Object *());
		VBoxContainer *error_list = Object::cast_to<VBoxContainer>(el_var.operator Object *());
		String summary = bubble->get_meta("_summary", "");
		if (header_label && error_list) {
			bool expanding = !error_list->is_visible();
			error_list->set_visible(expanding);
			String arrow = expanding ? String::utf8("\u25BC ") : String::utf8("\u25B6 ");
			header_label->set_text(arrow + summary);
		}
	}
}

// Shared helper: creates a collapsible error bubble for the chat transcript.
// p_summary: header text (e.g. "Including 2 errors"), shown with a triangle.
// p_errors: array of error dicts to display when expanded.
// p_bg_color, p_border_color, p_text_color: color scheme.
// p_error_format: enum to control per-entry formatting (declared near top of file).

static Control *_create_error_bubble_impl(
		const String &p_summary, const Array &p_errors,
		const Color &p_bg_color, const Color &p_border_color, const Color &p_text_color,
		ErrorBubbleFormat p_format) {
	// Outer panel
	PanelContainer *bubble = memnew(PanelContainer);
	Ref<StyleBoxFlat> style;
	style.instantiate();
	style->set_bg_color(p_bg_color);
	style->set_border_width_all(1);
	style->set_border_color(p_border_color);
	style->set_corner_radius_all(6 * EDSCALE);
	style->set_content_margin_all(8 * EDSCALE);
	bubble->add_theme_style_override("panel", style);
	bubble->set_h_size_flags(Control::SIZE_EXPAND_FILL);

	// Inner VBox for header + error list
	VBoxContainer *vbox = memnew(VBoxContainer);
	vbox->add_theme_constant_override("separation", 4 * EDSCALE);
	bubble->add_child(vbox);

	// Header label with triangle
	RichTextLabel *header_label = memnew(RichTextLabel);
	header_label->set_use_bbcode(false);
	header_label->set_fit_content(true);
	header_label->set_scroll_active(false);
	header_label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	header_label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	// Remove default RichTextLabel background/border to avoid double-border
	Ref<StyleBoxEmpty> header_empty_style;
	header_empty_style.instantiate();
	header_label->add_theme_style_override("normal", header_empty_style);
	header_label->add_theme_style_override("focus", header_empty_style);
	header_label->add_theme_color_override("default_color", p_text_color);
	header_label->add_theme_font_size_override("normal_font_size", 11 * EDSCALE);
	header_label->set_text(String::utf8("\u25B6 ") + p_summary); // ▶
	vbox->add_child(header_label);

	// Error list (hidden by default)
	VBoxContainer *error_list = memnew(VBoxContainer);
	error_list->set_visible(false);
	error_list->add_theme_constant_override("separation", 2 * EDSCALE);
	vbox->add_child(error_list);

	for (int i = 0; i < p_errors.size(); i++) {
		Dictionary err = p_errors[i];
		String entry_text;
		Color entry_color = p_text_color;
		entry_color.a = 0.8f;

		if (p_format == ERROR_BUBBLE_RUNTIME) {
			String severity = err.get("severity", "error");
			String message = err.get("message", "Unknown error");
			String script = err.get("script", "");
			int line = err.get("line", 0);
			int occurrences = err.get("occurrences", 1);

			entry_text = (severity == "warning") ? vformat("[W] %s", message) : vformat("[E] %s", message);
			if (!script.is_empty() && line > 0) {
				entry_text += vformat("  (%s:%d)", script.get_file(), line);
			}
			if (occurrences > 1) {
				entry_text += vformat("  x%d", occurrences);
			}
			if (severity == "warning") {
				entry_color = Color(1.0f, 0.85f, 0.4f, 0.8f);
			}
		} else {
			String message = err.get("message", "Unknown error");
			int line = err.get("line", 0);
			entry_text = (line > 0) ? vformat("Line %d: %s", line, message) : message;
		}

		Label *entry_label = memnew(Label);
		entry_label->set_text(entry_text);
		entry_label->add_theme_font_size_override("font_size", 10 * EDSCALE);
		entry_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
		entry_label->add_theme_color_override("font_color", entry_color);
		error_list->add_child(entry_label);
	}

	// Click to expand/collapse — store references on the bubble for the callback
	bubble->set_meta("_header_label", header_label);
	bubble->set_meta("_error_list", error_list);
	bubble->set_meta("_summary", p_summary);
	bubble->connect("gui_input", callable_mp_static(&_on_error_bubble_gui_input).bind(bubble));

	// Wrap in align container
	HBoxContainer *align = memnew(HBoxContainer);
	align->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	align->add_child(bubble);
	bubble->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	bubble->set_stretch_ratio(0.95f);
	return align;
}

Control *AIStatusPanel::_create_debug_context_bubble() {
	if (!debug_pill || !debug_pill->is_visible() || !debug_pill->is_enabled()) {
		return nullptr;
	}
	String summary = debug_pill->get_context_summary();
	if (summary.is_empty()) {
		return nullptr;
	}
	return _create_error_bubble_impl(
			summary, debug_pill->get_errors(),
			Color(0, 0, 0, 0), // bg
			Color(0.7f, 0.3f, 0.3f, 0.6f),     // border
			Color(1.0f, 0.5f, 0.45f, 1.0f),     // text
			ERROR_BUBBLE_RUNTIME);
}

Control *AIStatusPanel::_create_parse_error_bubble() {
	if (!parse_error_pill || !parse_error_pill->has_content()) {
		return nullptr;
	}
	String summary = parse_error_pill->get_context_summary();
	if (summary.is_empty()) {
		return nullptr;
	}
	return _create_error_bubble_impl(
			summary, parse_error_pill->get_errors(),
			Color(0, 0, 0, 0),  // bg
			Color(0.8f, 0.5f, 0.2f, 0.6f),     // border
			Color(1.0f, 0.6f, 0.3f, 1.0f),      // text
			ERROR_BUBBLE_PARSE);
}

Control *AIStatusPanel::_create_thinking_block(RichTextLabel **r_label) {
	// Thinking style (decided 7/28): italic, slightly lighter than body text,
	// one point smaller. Plain text in the transcript flow — no collapsible.
	// Vertical rhythm comes from message_list's separation, not per-block
	// margins, so every transcript element spaces identically.
	MarginContainer *wrapper = memnew(MarginContainer);
	wrapper->add_theme_constant_override("margin_left", AIColors::PADDING_SM * EDSCALE);
	wrapper->add_theme_constant_override("margin_right", 0);
	wrapper->set_h_size_flags(Control::SIZE_EXPAND_FILL);

	RichTextLabel *label = memnew(RichTextLabel);
	label->set_use_bbcode(false);
	label->set_fit_content(true);
	label->set_scroll_active(false);
	label->set_selection_enabled(true);
	label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	Ref<Font> italic_font = get_theme_font(SNAME("doc_italic"), SNAME("EditorFonts"));
	if (italic_font.is_valid()) {
		label->add_theme_font_override("normal_font", italic_font);
	}
	label->add_theme_color_override("default_color", AIColors::TEXT_SECONDARY);
	int base_size = get_theme_font_size(SNAME("font_size"), SNAME("Label"));
	if (base_size <= 0) {
		base_size = 13;
	}
	label->add_theme_font_size_override("normal_font_size", MAX(8, base_size - 1));
	wrapper->add_child(label);

	// Copy menu: the RichTextLabel consumes clicks (selection), so listen on
	// both the label and the wrapper; the menu reads the wrapper's meta.
	label->connect("gui_input", callable_mp(this, &AIStatusPanel::_on_message_bubble_gui_input).bind(wrapper));
	wrapper->connect("gui_input", callable_mp(this, &AIStatusPanel::_on_message_bubble_gui_input).bind(wrapper));

	if (r_label) {
		*r_label = label;
	}
	return wrapper;
}

Control *AIStatusPanel::_create_assistant_text_block(const String &p_text, RichTextLabel **r_label) {
	MarginContainer *wrapper = memnew(MarginContainer);
	wrapper->add_theme_constant_override("margin_left", AIColors::PADDING_SM * EDSCALE);
	wrapper->add_theme_constant_override("margin_right", 0);
	wrapper->set_h_size_flags(Control::SIZE_EXPAND_FILL);

	RichTextLabel *label = memnew(RichTextLabel);
	label->set_use_bbcode(true);
	label->set_fit_content(true);
	label->set_scroll_active(false);
	label->set_selection_enabled(true);
	label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	label->add_theme_color_override("default_color", AIColors::TEXT_PRIMARY);
	label->set_text(_markdown_to_bbcode(p_text));
	wrapper->add_child(label);

	wrapper->set_meta("_bubble_plain_text", p_text);
	// The RichTextLabel consumes clicks for selection; listen on both it and
	// the wrapper so right-click copy works anywhere on the block.
	label->connect("gui_input", callable_mp(this, &AIStatusPanel::_on_message_bubble_gui_input).bind(wrapper));
	wrapper->connect("gui_input", callable_mp(this, &AIStatusPanel::_on_message_bubble_gui_input).bind(wrapper));
	if (r_label) {
		*r_label = label;
	}
	return wrapper;
}

Control *AIStatusPanel::_create_plan_panel(const String &p_text) {
	MarginContainer *wrapper = memnew(MarginContainer);
	wrapper->add_theme_constant_override("margin_left", AIColors::PADDING_SM * EDSCALE);
	wrapper->add_theme_constant_override("margin_right", 0);
	wrapper->set_h_size_flags(Control::SIZE_EXPAND_FILL);

	PanelContainer *panel = memnew(PanelContainer);
	panel->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	Ref<StyleBoxFlat> style;
	style.instantiate();
	style->set_bg_color(AIColors::ASSISTANT_BG);
	style->set_corner_radius_all(AIColors::CORNER_RADIUS_LG * EDSCALE);
	style->set_content_margin_all(AIColors::PADDING_MD * EDSCALE);
	// Deliberately no border: the shade shift alone marks the plan.
	panel->add_theme_style_override("panel", style);
	wrapper->add_child(panel);

	RichTextLabel *label = memnew(RichTextLabel);
	label->set_use_bbcode(true);
	label->set_fit_content(true);
	label->set_scroll_active(false);
	label->set_selection_enabled(true);
	label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	label->add_theme_color_override("default_color", AIColors::TEXT_PRIMARY);
	label->set_text(_markdown_to_bbcode(p_text));
	panel->add_child(label);

	wrapper->set_meta("_bubble_plain_text", p_text);
	label->connect("gui_input", callable_mp(this, &AIStatusPanel::_on_message_bubble_gui_input).bind(wrapper));
	wrapper->connect("gui_input", callable_mp(this, &AIStatusPanel::_on_message_bubble_gui_input).bind(wrapper));
	return wrapper;
}

void AIStatusPanel::_append_assistant_blocks(const String &p_text, Control *p_insert_before) {
	if (!message_list) {
		return;
	}
	// Split into normal and [PLAN]-delimited segments (delimiters on their own
	// lines, stripped from display; an unclosed [PLAN] runs to the end).
	PackedStringArray lines = p_text.split("\n");
	String segment;
	bool in_plan = false;
	Vector<Control *> blocks;
	for (int i = 0; i <= lines.size(); i++) {
		bool at_end = i == lines.size();
		String stripped = at_end ? String() : lines[i].strip_edges();
		bool is_open = stripped == "[PLAN]";
		bool is_close = stripped == "[/PLAN]";
		if (at_end || is_open || is_close) {
			String seg = segment.strip_edges();
			if (!seg.is_empty()) {
				blocks.push_back(in_plan ? _create_plan_panel(seg) : _create_assistant_text_block(seg));
			}
			segment = String();
			if (is_open) {
				in_plan = true;
			} else if (is_close) {
				in_plan = false;
			}
			continue;
		}
		segment += lines[i] + "\n";
	}
	for (Control *block : blocks) {
		message_list->add_child(block);
		if (p_insert_before) {
			message_list->move_child(block, p_insert_before->get_index());
		}
	}
}

void AIStatusPanel::_on_harness_thinking_delta(const String &p_delta) {
	if (!message_list) {
		return;
	}
	if (!harness_thinking_block) {
		harness_thinking_text = String();
		harness_thinking_block = _create_thinking_block(&harness_thinking_label);
		if (pending_message) {
			int idx = pending_message->get_index();
			message_list->add_child(harness_thinking_block);
			message_list->move_child(harness_thinking_block, idx);
		} else {
			message_list->add_child(harness_thinking_block);
		}
	}
	harness_thinking_text += p_delta;
	harness_thinking_label->set_text(harness_thinking_text);
	harness_thinking_block->set_meta("_bubble_plain_text", harness_thinking_text);
	// No direct scroll: stick-to-bottom is handled by the range-changed hook.
}

void AIStatusPanel::_finalize_harness_thinking() {
	// Persist the finished thinking phase so it survives chat switches and
	// editor restarts (rendered on reload by the injection branch).
	if (!harness_thinking_text.is_empty() && chat_store.is_valid()) {
		Dictionary thinking_item;
		thinking_item["type"] = "thinking";
		thinking_item["text"] = harness_thinking_text;
		chat_store->append_item(thinking_item);
	}
	// The block stays in the transcript as styled text; just detach it from
	// the streaming lifecycle so the next thinking phase gets a fresh one.
	harness_thinking_block = nullptr;
	harness_thinking_label = nullptr;
	harness_thinking_text = String();
}

void AIStatusPanel::_append_thinking_ui(const String &p_text) {
	// Note: thinking content is not persisted in v2.1 — it's ephemeral UI only
	if (!message_list || p_text.is_empty()) {
		return;
	}
	RichTextLabel *label = nullptr;
	Control *block = _create_thinking_block(&label);
	label->set_text(p_text);
	block->set_meta("_bubble_plain_text", p_text);
	if (pending_message) {
		int idx = pending_message->get_index();
		message_list->add_child(block);
		message_list->move_child(block, idx);
	} else {
		message_list->add_child(block);
	}
}

void AIStatusPanel::_on_orchestrator_narration(const String &p_text) {
	// No-op in v2.1 — assistant content is handled via _on_orchestrator_assistant_item
	(void)p_text;
}

void AIStatusPanel::_on_orchestrator_assistant_item(const Dictionary &p_item) {
	// Any completed item (text or tool call) ends the current thinking phase;
	// the next reasoning delta starts a fresh block BELOW this item, keeping
	// the transcript in true chronological order (thinking→tool→thinking→...).
	_finalize_harness_thinking();

	// Persist the canonical assistant item (text + tool_call blocks)
	if (chat_store.is_valid()) {
		chat_store->append_item(p_item);
	}

	// Render text content
	if (!message_list) {
		return;
	}
	Array content = p_item.get("content", Array());
	for (int i = 0; i < content.size(); i++) {
		Dictionary block = content[i];
		if (String(block.get("type", "")) == "text") {
			String text = block.get("text", "");
			if (text.strip_edges().is_empty()) {
				_reset_harness_stream();
				break;
			}
			if (use_harness_mode && harness_streaming && harness_stream_block && harness_stream_rich) {
				if (text.contains("[PLAN]")) {
					// Plan sections re-render as distinct panels: swap the
					// streamed block for the split layout (it sits at the
					// end; the dots were removed when streaming began).
					message_list->remove_child(harness_stream_block);
					memdelete(harness_stream_block);
					harness_stream_block = nullptr;
					harness_stream_rich = nullptr;
					_append_assistant_blocks(text, nullptr);
				} else {
					// The streamed block IS the final rendering: set the
					// authoritative text and release it to the transcript.
					harness_stream_rich->set_text(_markdown_to_bbcode(text));
					harness_stream_block->set_meta("_bubble_plain_text", text);
					harness_stream_block = nullptr;
					harness_stream_rich = nullptr;
				}
				harness_streaming = false;
				harness_stream_text = String();
				_show_pending_message();
				break;
			}
			// No live stream to finalize (legacy loop, or a raced item).
			_append_assistant_blocks(text, pending_message);
			break; // one text item per assistant item
		}
	}

	// Spawn a pending tool card per tool_call block so the user sees the tool is
	// running immediately, not just when it completes. The reload path in
	// _rebuild_message_list uses the same pattern against the persisted assistant
	// item; this is the live-run counterpart. Cards are paired to results by
	// call_id in _append_tool_result_ui.
	for (int i = 0; i < content.size(); i++) {
		Dictionary block = content[i];
		if (String(block.get("type", "")) != "tool_call") {
			continue;
		}
		String call_id = block.get("id", "");
		String tool_name = block.get("name", "");
		// update_todos is filtered from the transcript in _on_orchestrator_tool_result;
		// don't create a pending card for it either or it will linger forever.
		if (tool_name == "update_todos") {
			continue;
		}
		if (call_id.is_empty() || pending_tool_entries.has(call_id)) {
			continue;
		}
		Dictionary placeholder;
		placeholder["type"] = tool_name;
		placeholder["tool_name"] = tool_name;
		placeholder["args"] = block.get("args", Dictionary());
		placeholder["action_id"] = call_id;
		placeholder["status"] = "pending";
		placeholder["tokens"] = 0;

		ToolCollapsibleEntry *entry = Object::cast_to<ToolCollapsibleEntry>(_create_tool_result_ui(placeholder));
		if (entry) {
			if (pending_message) {
				int idx = pending_message->get_index();
				message_list->add_child(entry);
				message_list->move_child(entry, idx);
			} else {
				message_list->add_child(entry);
			}
			pending_tool_entries[call_id] = entry;
		}
	}
	if (!pending_tool_entries.is_empty() && pending_tool_timer && pending_tool_timer->is_stopped()) {
		pending_tool_timer->start();
	}
}

Control *AIStatusPanel::_create_narration_bubble(const String &p_text) {
	PanelContainer *bubble = memnew(PanelContainer);
	bubble->set_h_size_flags(SIZE_EXPAND_FILL);
	bubble->set_stretch_ratio(0.95);

	Ref<StyleBoxFlat> style;
	style.instantiate();
	style->set_corner_radius_all(AIColors::CORNER_RADIUS_LG * EDSCALE);
	style->set_content_margin_all(AIColors::PADDING_MD * EDSCALE);
	style->set_bg_color(AIColors::ASSISTANT_BG);
	style->set_border_width(SIDE_LEFT, 3 * EDSCALE);
	style->set_border_color(AIColors::ACCENT_BLUE_MUTED);
	bubble->add_theme_style_override("panel", style);

	VBoxContainer *vbox = memnew(VBoxContainer);
	vbox->add_theme_constant_override("separation", AIColors::PADDING_XS * EDSCALE);
	bubble->add_child(vbox);

	Label *header = memnew(Label);
	header->set_text(TTR("Planning..."));
	header->add_theme_color_override("font_color", AIColors::TEXT_MUTED);
	header->add_theme_font_size_override("font_size", 11 * EDSCALE);
	vbox->add_child(header);

	RichTextLabel *label = memnew(RichTextLabel);
	label->set_use_bbcode(true);
	label->set_fit_content(true);
	label->set_scroll_active(false);
	label->set_selection_enabled(true);
	label->add_theme_color_override("default_color", AIColors::TEXT_SECONDARY);
	label->add_theme_color_override("background_color", Color(0, 0, 0, 0));
	Ref<StyleBoxEmpty> empty_style;
	empty_style.instantiate();
	label->add_theme_style_override("normal", empty_style);
	label->add_theme_style_override("focus", empty_style);
	_append_message_content(label, p_text);
	vbox->add_child(label);

	// Wrap in HBoxContainer for consistent margins (same as assistant bubbles)
	HBoxContainer *align = memnew(HBoxContainer);
	align->set_h_size_flags(SIZE_EXPAND_FILL);
	align->add_child(bubble);
	Control *spacer = memnew(Control);
	spacer->set_h_size_flags(SIZE_EXPAND_FILL);
	spacer->set_stretch_ratio(0.05);
	align->add_child(spacer);
	return align;
}

Control *AIStatusPanel::_create_cancel_notice() {
	Label *lbl = memnew(Label);
	lbl->set_text(TTR("User interrupted the conversation."));
	lbl->set_h_size_flags(SIZE_EXPAND_FILL);
	lbl->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_LEFT);
	lbl->add_theme_color_override("font_color", AIColors::TEXT_MUTED);
	lbl->add_theme_font_size_override("font_size", 13 * EDSCALE);
	return lbl;
}

Control *AIStatusPanel::_create_error_notice(const String &p_text) {
	Label *lbl = memnew(Label);
	lbl->set_text(p_text);
	lbl->set_h_size_flags(SIZE_EXPAND_FILL);
	lbl->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_LEFT);
	lbl->add_theme_color_override("font_color", AIColors::TEXT_MUTED);
	lbl->add_theme_font_size_override("font_size", 13 * EDSCALE);
	lbl->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	return lbl;
}

void AIStatusPanel::_on_orchestrator_thinking(const String &p_text) {
	// No-op in v2.1 — thinking_ready signal removed from orchestrator
	(void)p_text;
}

void AIStatusPanel::_on_orchestrator_progress(const String &p_status, int p_turn) {
	// Progress now only handles status bar updates — thinking comes via thinking_ready signal
	if (status_label) {
		status_label->set_text(vformat("Turn %d: %s", p_turn, p_status));
	}
	_refresh_context_usage();

	print_verbose(vformat("AI Chat Panel: Agentic progress (turn %d): %s", p_turn, p_status));
}

void AIStatusPanel::_on_orchestrator_tool_result(const Dictionary &p_tool_result) {
	print_line(vformat("AIStatusPanel: _on_orchestrator_tool_result called - type=%s, status=%s",
		String(p_tool_result.get("type", "unknown")), String(p_tool_result.get("status", "unknown"))));

	// update_todos is reflected in the todo panel, not the transcript — skip the
	// card and token count, but fall through to store persistence: the assistant's
	// tool_call needs a matching tool item when history is rebuilt for the next
	// request, or strict OpenAI-compatible providers reject it with HTTP 400.
	const bool suppress_transcript_card = String(p_tool_result.get("type", "")) == "update_todos";

	if (!suppress_transcript_card) {
		_run_token_total += (int)p_tool_result.get("tokens", 0);

		// Append tool result to chat transcript
		_append_tool_result_ui(p_tool_result);
	}

	// Persist to store as canonical tool item (save screenshot to disk, reference by filename)
	if (chat_store.is_valid()) {
		String call_id = p_tool_result.get("action_id", "");
		String tool_name = p_tool_result.get("tool_name", "");
		String status = p_tool_result.get("status", "error");

		Dictionary content;
		content["status"] = status;
		content["tool_name"] = tool_name;
		content["args"] = p_tool_result.get("args", Dictionary());

		if (status == String("success")) {
			Dictionary result = p_tool_result.get("result", Dictionary());
			// Save screenshots to disk and replace base64 with filename references.
			// Applies to any tool that returns image data — keeps JSONL files small
			// and lets the reload path rehydrate images from PNGs on disk.
			//   - result.screenshot_b64           → result.screenshot (filename)
			//   - result.screenshots[].screenshot_b64 → result.screenshots[].screenshot
			if (result.has("screenshot_b64")) {
				result = result.duplicate();
				String filename = chat_store->save_screenshot(call_id, result["screenshot_b64"]);
				result.erase("screenshot_b64");
				if (!filename.is_empty()) {
					result["screenshot"] = filename;
				}
			}
			if (result.has("screenshots") && result["screenshots"].get_type() == Variant::ARRAY) {
				Array shots = result["screenshots"];
				Vector<String> b64s;
				b64s.resize(shots.size());
				for (int i = 0; i < shots.size(); i++) {
					Dictionary entry = shots[i];
					b64s.write[i] = entry.get("screenshot_b64", "");
				}
				Vector<String> filenames = chat_store->save_screenshots(call_id, b64s);
				Array shots_persisted;
				shots_persisted.resize(shots.size());
				for (int i = 0; i < shots.size(); i++) {
					Dictionary stripped = ((Dictionary)shots[i]).duplicate();
					stripped.erase("screenshot_b64");
					if (i < filenames.size() && !filenames[i].is_empty()) {
						stripped["screenshot"] = filenames[i];
					}
					shots_persisted[i] = stripped;
				}
				result = result.duplicate();
				result["screenshots"] = shots_persisted;
			}
			content["result"] = result;
		} else if (status == String("cancelled")) {
			content["reason"] = p_tool_result.get("reason", "user_cancelled_run");
		} else {
			content["error"] = p_tool_result.get("error", Dictionary());
		}

		chat_store->append_item(AIChatStore::make_tool_item(call_id, content));
	}

	// Check for parse errors in script tool results and update pill + AI singleton
	if (parse_error_pill) {
		String tool_name = p_tool_result.get("tool_name", "");
		if (tool_name == "update_script" || tool_name == "create_script") {
			Dictionary result = p_tool_result.get("result", Dictionary());
			String file_path = result.get("file_path", "");
			Array parse_errors = result.get("parse_errors", Array());

			AI *ai = AI::get_singleton();
			if (!parse_errors.is_empty()) {
				parse_error_pill->set_errors(file_path, parse_errors);
				if (ai) {
					ai->set_parse_errors(file_path, parse_errors);
				}
			} else if (!file_path.is_empty()) {
				// Script compiled clean — clear any previous errors for this file
				parse_error_pill->clear_for_file(file_path);
				if (ai) {
					ai->clear_parse_errors();
				}
			}
		}
	}

	// Refresh context usage to reflect the new tool result added to the store
	_refresh_context_usage();
}

void AIStatusPanel::_on_orchestrator_complete(bool p_success, const String &p_final_message) {
	print_line(vformat("AIStatusPanel: _on_orchestrator_complete called - success=%s, message_length=%d", p_success ? "true" : "false", p_final_message.length()));

	// A cancelled run may leave an approval prompt showing (the driver has
	// already answered codex); restore the composer.
	if (!approval_queue.is_empty() || (approval_panel && approval_panel->is_visible())) {
		approval_queue.clear();
		_show_next_approval();
	}

	// Session continuity: remember which codex thread backs this chat so a
	// restarted editor resumes it (history intact) instead of starting fresh.
	if (use_harness_mode && harness_driver.is_valid() && chat_store.is_valid()) {
		String thread_id = harness_driver->get_thread_id();
		if (!thread_id.is_empty()) {
			EditorSettings::get_singleton()->set_project_metadata(
					"ai_harness_threads", chat_store->get_chat_id(), thread_id);
		}
	}

	// Remove pending message
	_remove_pending_message();

	_insert_token_total_label();
	_run_token_total = 0;

	// Successful final assistant message was already stored and rendered by _on_orchestrator_assistant_item.
	const bool was_cancelled = !p_success && p_final_message.begins_with("<turn_cancelled>");
	if (!p_success && !p_final_message.is_empty()) {
		if (was_cancelled) {
			// Cancelled: persist as user-role message (model-visible context) and show bold inline text.
			if (chat_store.is_valid()) {
				Dictionary data;
				data["role"] = "user";
				data["content"] = p_final_message;
				chat_store->append_item(data);
			}
			if (message_list) {
				Control *notice = _create_cancel_notice();
				if (notice) {
					message_list->add_child(notice);
				}
			}
		} else {
			// Error: persist as system-role message so it survives reload
			if (chat_store.is_valid()) {
				Dictionary data;
				data["role"] = "system";
				data["type"] = "error";
				data["content"] = p_final_message;
				chat_store->append_item(data);
			}
			if (message_list) {
				Control *notice = _create_error_notice(p_final_message);
				if (notice) {
					message_list->add_child(notice);
				}
			}
		}
	}

	// Update state
	is_waiting_for_response = false;
	_set_run_state(STATE_IDLE);

	// Flush any dangling pending tool cards — can happen if a run was cancelled
	// or errored before every tool_call produced a result.
	if (!pending_tool_entries.is_empty()) {
		for (KeyValue<String, ToolCollapsibleEntry *> &kv : pending_tool_entries) {
			if (!kv.value) {
				continue;
			}
			Dictionary cancelled;
			cancelled["type"] = "unknown";
			cancelled["status"] = "error";
			Dictionary err;
			err["message"] = p_success ? String("Tool did not produce a result.") : String("Run ended before this tool completed.");
			cancelled["error"] = err;
			kv.value->update_from_tool_result(cancelled);
		}
		pending_tool_entries.clear();
	}
	if (pending_tool_timer) {
		pending_tool_timer->stop();
	}

	// Check if context was truncated — if so, mark as exhausted and show a notice
	if (context_was_truncated && !context_exhausted) {
		context_exhausted = true;
		// Insert a system notice bubble at the bottom of the transcript
		if (message_list) {
			Control *notice = _create_narration_bubble(TTR("The context is full. Please start a new chat to continue."));
			if (notice) {
				message_list->add_child(notice);
				_scroll_to_bottom();
			}
		}
		_update_send_button_state();
	}

	// Reset status label
	if (status_label) {
		status_label->set_text(p_success ? TTR("Ready") : TTR("Cancelled"));
	}

	// Update context usage to reflect the new assistant message added to the store
	_refresh_context_usage();

	// Queued messages that were never consumed by the run:
	// - Cancelled run: the user is reconsidering — return their text to the
	//   composer for editing instead of auto-starting a new run.
	// - Normal completion (e.g. the model answered before its next request
	//   could consume them): send the next one as a fresh run, as before.
	if (!message_queue.is_empty()) {
		if (was_cancelled) {
			if (prompt_edit) {
				String restored;
				for (const QueuedMessage &qm : message_queue) {
					if (!restored.is_empty()) {
						restored += "\n\n";
					}
					restored += qm.text;
				}
				String existing = prompt_edit->get_text();
				prompt_edit->set_text(existing.is_empty() ? restored : restored + "\n\n" + existing);
				prompt_edit->grab_focus();
				prompt_edit->set_caret_line(prompt_edit->get_line_count() - 1);
				prompt_edit->set_caret_column(prompt_edit->get_line(prompt_edit->get_line_count() - 1).length());
			}
			print_line(vformat("AIStatusPanel: Run cancelled, returned %d queued message(s) to composer.", message_queue.size()));
			message_queue.clear();
			_update_queue_ui();
			_update_send_button_state();
		} else {
			print_line(vformat("AIStatusPanel: Run complete, %d messages in queue. Starting next...", message_queue.size()));
			// Use call_deferred to avoid re-entrancy issues
			callable_mp(this, &AIStatusPanel::_dequeue_and_run_next).call_deferred();
		}
	}
}

void AIStatusPanel::_on_thinking_dot_tick() {
	static const char *states[] = { "Thinking", "Thinking.", "Thinking..", "Thinking..." };
	thinking_dot_state = (thinking_dot_state + 1) % 4;
	if (pending_label) {
		pending_label->set_text(states[thinking_dot_state]);
	}
}

void AIStatusPanel::_on_pending_tool_spinner_tick() {
	if (pending_tool_entries.is_empty()) {
		if (pending_tool_timer) {
			pending_tool_timer->stop();
		}
		return;
	}
	static const char *FRAMES[] = { "\xE2\xA0\x8B", "\xE2\xA0\x99", "\xE2\xA0\xB9", "\xE2\xA0\xB8",
			"\xE2\xA0\xBC", "\xE2\xA0\xB4", "\xE2\xA0\xA6", "\xE2\xA0\xA7", "\xE2\xA0\x87", "\xE2\xA0\x8F" };
	pending_tool_spinner_frame = (pending_tool_spinner_frame + 1) % 10;
	String glyph = String::utf8(FRAMES[pending_tool_spinner_frame]);
	for (KeyValue<String, ToolCollapsibleEntry *> &kv : pending_tool_entries) {
		if (kv.value) {
			kv.value->set_pending_glyph(glyph);
		}
	}
}

void AIStatusPanel::_show_pending_message() {
	if (!message_list || pending_message) {
		return;
	}

	// Plain label — no bubble, wrapped in MarginContainer for left indent
	MarginContainer *wrapper = memnew(MarginContainer);
	wrapper->add_theme_constant_override("margin_left", AIColors::PADDING_SM * EDSCALE);
	wrapper->add_theme_constant_override("margin_top", AIColors::PADDING_XS * EDSCALE);
	wrapper->add_theme_constant_override("margin_bottom", AIColors::PADDING_XS * EDSCALE);
	wrapper->add_theme_constant_override("margin_right", 0);

	Label *label = memnew(Label);
	label->set_text("Thinking");
	label->add_theme_color_override("font_color", AIColors::TEXT_MUTED);
	wrapper->add_child(label);

	pending_message = wrapper;
	pending_label = label;
	thinking_dot_state = 0;
	message_list->add_child(pending_message);

	if (thinking_dot_timer) {
		thinking_dot_timer->start();
	}
}

void AIStatusPanel::_remove_pending_message() {
	if (thinking_dot_timer) {
		thinking_dot_timer->stop();
	}
	harness_streaming = false;
	harness_stream_text = String();
	harness_stream_block = nullptr; // Block stays in the transcript (partial text kept on cancel).
	harness_stream_rich = nullptr;
	_finalize_harness_thinking();
	pending_label = nullptr;
	if (pending_message && message_list) {
		message_list->remove_child(pending_message);
		memdelete(pending_message);
		pending_message = nullptr;
	}
}

// ============================================================================
// Message Bubble Context Menu (right-click → Copy text)
// ============================================================================

void AIStatusPanel::_on_message_bubble_gui_input(const Ref<InputEvent> &p_event, Control *p_bubble) {
	Ref<InputEventMouseButton> mb = p_event;
	if (!mb.is_valid() || mb->get_button_index() != MouseButton::RIGHT || !mb->is_pressed()) {
		return;
	}
	if (!p_bubble || !bubble_context_menu) {
		return;
	}
	_bubble_menu_text = p_bubble->get_meta("_bubble_plain_text", "");
	if (_bubble_menu_text.is_empty()) {
		return;
	}
	Vector2i screen_pos = p_bubble->get_screen_position() + Vector2i(mb->get_position());
	bubble_context_menu->set_position(screen_pos);
	bubble_context_menu->reset_size();
	bubble_context_menu->popup();
}

void AIStatusPanel::_on_bubble_menu_id_pressed(int p_id) {
	if (p_id != 0 || _bubble_menu_text.is_empty()) {
		return;
	}
	DisplayServer *ds = DisplayServer::get_singleton();
	if (ds) {
		ds->clipboard_set(_bubble_menu_text);
	}
}

// ============================================================================
// Rewind Functionality
// ============================================================================

void AIStatusPanel::_on_rewind_clicked(int64_t p_message_id) {
	// Safety check: don't allow rewind during active run
	if (run_state != STATE_IDLE) {
		WARN_PRINT("AI Chat Panel: Cannot rewind while a run is in progress. Stop the current run first.");
		return;
	}

	// Check if checkpoint exists for this message
	if (!chat_store.is_valid()) {
		return;
	}

	const ChatCheckpoint *cp = chat_store->get_checkpoint_for_ts(p_message_id);
	if (!cp) {
		WARN_PRINT(vformat("AI Chat Panel: No checkpoint available for message %d. Checkpoints are created after successful runs.", p_message_id));
		return;
	}

	// Clear any pending edit state
	_cancel_pending_edit();

	// Store pending rewind info
	pending_rewind_message_id = p_message_id;
	pending_rewind_checkpoint_id = cp->checkpoint_id;

	// Store undo info for revert option
	undo_target_for_edit = cp->undo_action_index;
	undo_available_for_edit = cp->undo_revert_available;

	// Reset dialog for rewind mode
	if (rewind_dialog_label) {
		rewind_dialog_label->set_text(TTR("Rewinding will clear the messages after this one and let you continue from here."));
	}
	if (rewind_dialog) {
		rewind_dialog->set_title(TTR("Rewind conversation?"));
	}

	// Update revert button availability
	if (continue_revert_button) {
		continue_revert_button->set_disabled(!cp->undo_revert_available);
		if (!cp->undo_revert_available) {
			continue_revert_button->set_tooltip_text(TTR("Project revert not available for this checkpoint"));
		} else {
			continue_revert_button->set_tooltip_text("");
		}
	}

	// Show confirmation dialog
	if (rewind_dialog) {
		rewind_dialog->reset_size();
		rewind_dialog->popup_centered();
	}
}

void AIStatusPanel::_on_dialog_cancel() {
	if (rewind_dialog) {
		rewind_dialog->hide();
	}
	// Don't clear state - user can try again
}

void AIStatusPanel::_on_dialog_continue_no_revert() {
	if (rewind_dialog) {
		rewind_dialog->hide();
	}

	// Check "Don't ask again"
	if (dont_ask_again_checkbox && dont_ask_again_checkbox->is_pressed()) {
		skip_edit_send_dialog = true;
	}

	if (is_pending_edit_send) {
		// Edit mode: send without reverting
		is_pending_edit_send = false;
		undo_target_for_edit = -1;
		undo_available_for_edit = false;

		// Send the message
		if (prompt_edit) {
			String prompt_text = prompt_edit->get_text().strip_edges();
			if (!prompt_text.is_empty()) {
				prompt_edit->set_text("");
				_update_send_button_state();
				_start_run(prompt_text);
			}
		}
	} else {
		// Rewind mode: rewind without reverting project
		if (!pending_rewind_checkpoint_id.is_empty()) {
			_perform_rewind(pending_rewind_checkpoint_id, false);
			pending_rewind_message_id = 0;
			pending_rewind_checkpoint_id = "";
		}
	}

	undo_target_for_edit = -1;
	undo_available_for_edit = false;
}

void AIStatusPanel::_on_dialog_continue_revert() {
	if (rewind_dialog) {
		rewind_dialog->hide();
	}

	// Check "Don't ask again" - but reverting, so don't skip future dialogs
	// (Only skip if user chooses no-revert)

	if (is_pending_edit_send) {
		// Edit mode: revert then send
		if (undo_available_for_edit && undo_target_for_edit >= 0) {
			EditorUndoRedoManager *urm = EditorUndoRedoManager::get_singleton();
			if (urm) {
				UndoRedo *ur = urm->get_history_undo_redo(EditorUndoRedoManager::GLOBAL_HISTORY);
				if (ur) {
					int current_action = ur->get_current_action();
					if (current_action > undo_target_for_edit) {
						int undos_needed = current_action - undo_target_for_edit;
						print_line(vformat("AI Chat Panel: Reverting project for edit - undoing %d actions", undos_needed));
						for (int i = 0; i < undos_needed; i++) {
							urm->undo();
						}
					}
				}
			}
		}

		is_pending_edit_send = false;
		undo_target_for_edit = -1;
		undo_available_for_edit = false;

		// Send the message
		if (prompt_edit) {
			String prompt_text = prompt_edit->get_text().strip_edges();
			if (!prompt_text.is_empty()) {
				prompt_edit->set_text("");
				_update_send_button_state();
				_start_run(prompt_text);
			}
		}
	} else {
		// Rewind mode: rewind with reverting project
		if (!pending_rewind_checkpoint_id.is_empty()) {
			_perform_rewind(pending_rewind_checkpoint_id, true);
			pending_rewind_message_id = 0;
			pending_rewind_checkpoint_id = "";
		}
	}

	undo_target_for_edit = -1;
	undo_available_for_edit = false;
}

void AIStatusPanel::_perform_rewind(const String &p_checkpoint_id, bool p_revert_project) {
	if (!chat_store.is_valid()) {
		return;
	}

	print_line(vformat("AI Chat Panel: Performing rewind to checkpoint '%s' (revert project: %s)",
			p_checkpoint_id, p_revert_project ? "yes" : "no"));

	// Get checkpoint before truncation (we need undo info)
	const ChatCheckpoint *cp = nullptr;
	const Vector<ChatCheckpoint> &checkpoints = chat_store->get_checkpoints();
	for (int i = 0; i < checkpoints.size(); i++) {
		if (checkpoints[i].checkpoint_id == p_checkpoint_id) {
			cp = &checkpoints[i];
			break;
		}
	}

	// Revert project state if requested
	if (p_revert_project && cp && cp->undo_revert_available) {
		_revert_project_to_checkpoint(*cp);
	}

	// Truncate transcript
	if (!chat_store->truncate_to_checkpoint(p_checkpoint_id)) {
		ERR_PRINT("AI Chat Panel: Failed to truncate transcript to checkpoint");
		return;
	}

	// Rebuild UI
	_rebuild_message_list();
	_refresh_context_usage();

	// Update status
	if (status_label) {
		status_label->set_text(TTR("Rewound"));
	}

	print_line("AI Chat Panel: Rewind complete");
}

void AIStatusPanel::_revert_project_to_checkpoint(const ChatCheckpoint &p_checkpoint) {
	if (!p_checkpoint.undo_revert_available || p_checkpoint.undo_action_index < 0) {
		WARN_PRINT("AI Chat Panel: Project revert not available for this checkpoint");
		return;
	}

	EditorUndoRedoManager *urm = EditorUndoRedoManager::get_singleton();
	if (!urm) {
		WARN_PRINT("AI Chat Panel: EditorUndoRedoManager not available");
		return;
	}

	UndoRedo *ur = urm->get_history_undo_redo(EditorUndoRedoManager::GLOBAL_HISTORY);
	if (!ur) {
		WARN_PRINT("AI Chat Panel: UndoRedo history not available");
		return;
	}

	int current_action = ur->get_current_action();
	int target_action = p_checkpoint.undo_action_index;

	if (current_action > target_action) {
		int undos_needed = current_action - target_action;
		print_line(vformat("AI Chat Panel: Reverting project - undoing %d actions (current: %d, target: %d)",
				undos_needed, current_action, target_action));

		for (int i = 0; i < undos_needed; i++) {
			urm->undo();
		}

		print_line("AI Chat Panel: Project revert complete");
	} else {
		print_line(vformat("AI Chat Panel: No undo needed (current action %d <= target %d)", current_action, target_action));
	}
}

void AIStatusPanel::_on_checkpoint_recommended(int64_t p_user_message_id) {
	if (!chat_store.is_valid()) {
		return;
	}

	// Get current UndoRedo action index
	int undo_index = -1;
	bool undo_available = false;

	EditorUndoRedoManager *urm = EditorUndoRedoManager::get_singleton();
	if (urm) {
		UndoRedo *ur = urm->get_history_undo_redo(EditorUndoRedoManager::GLOBAL_HISTORY);
		if (ur) {
			undo_index = ur->get_current_action();
			undo_available = true;
		}
	}

	// Create checkpoint
	chat_store->create_checkpoint(p_user_message_id, undo_index, undo_available);
}

// ============================================================================
// Edit Functionality (Immediate truncate + prefill, dialog on send)
// ============================================================================

void AIStatusPanel::_on_edit_clicked(int64_t p_message_id) {
	// Safety check: don't allow edit during active run
	if (run_state != STATE_IDLE) {
		WARN_PRINT("AI Chat Panel: Cannot edit while a run is in progress. Stop the current run first.");
		return;
	}

	if (!chat_store.is_valid()) {
		return;
	}

	// Find the item by ts
	int msg_index = chat_store->find_item_index_by_ts(p_message_id);
	if (msg_index < 0) {
		ERR_PRINT(vformat("AI Chat Panel: Item with ts %d not found", p_message_id));
		return;
	}

	const Vector<HistoryItem> &items = chat_store->get_items();
	const HistoryItem &target_item = items[msg_index];

	// Only allow editing user messages
	if (target_item.role() != "user") {
		WARN_PRINT("AI Chat Panel: Can only edit user messages");
		return;
	}

	// Get the message content before truncation
	String edit_content = target_item.data.get("content", "");

	// Find checkpoint of PREVIOUS user message for project revert option
	// Store undo info for later (when user confirms send)
	undo_target_for_edit = -1;
	undo_available_for_edit = false;
	for (int i = msg_index - 1; i >= 0; i--) {
		if (items[i].role() == "user") {
			const ChatCheckpoint *prev_cp = chat_store->get_checkpoint_for_ts(items[i].ts);
			if (prev_cp && prev_cp->undo_revert_available) {
				undo_target_for_edit = prev_cp->undo_action_index;
				undo_available_for_edit = true;
				break;
			}
		}
	}

	// Clear any queued messages (edit implies starting fresh from this point)
	if (!message_queue.is_empty()) {
		print_line(vformat("AI Chat Panel: Clearing %d queued messages for edit", message_queue.size()));
		message_queue.clear();
		_update_queue_ui();
	}

	// Truncate to BEFORE the message (not including it)
	if (!chat_store->truncate_to_index(msg_index)) {
		ERR_PRINT("AI Chat Panel: Failed to truncate transcript for edit");
		return;
	}

	// Mark that we're in pending edit send mode
	is_pending_edit_send = true;

	// Rebuild UI to show truncated state
	_rebuild_message_list();

	// Prefill the prompt with the message content
	if (prompt_edit) {
		prompt_edit->set_text(edit_content);
		prompt_edit->grab_focus();
		// Move cursor to end
		prompt_edit->set_caret_line(prompt_edit->get_line_count() - 1);
		prompt_edit->set_caret_column(prompt_edit->get_line(prompt_edit->get_line_count() - 1).length());
	}

	// Update send button state (should be enabled since there's text)
	_update_send_button_state();

	// Update status
	if (status_label) {
		status_label->set_text(TTR("Editing..."));
	}

	print_line(vformat("AI Chat Panel: Edit started for message %d, ready for user to modify and send", p_message_id));
}

void AIStatusPanel::_cancel_pending_edit() {
	is_pending_edit_send = false;
	undo_target_for_edit = -1;
	undo_available_for_edit = false;

	if (status_label) {
		status_label->set_text(TTR("Ready"));
	}
}

void AIStatusPanel::_on_harness_assistant_delta(const String &p_delta) {
	// Answer text starting means the thinking phase (if any) is over.
	_finalize_harness_thinking();
	if (!message_list) {
		return;
	}
	if (!harness_stream_block) {
		// Swap the dots for a live rich-text block: markdown renders
		// best-effort as it streams (unclosed markers stay literal until
		// their closing pair arrives).
		_remove_pending_message();
		harness_stream_block = _create_assistant_text_block(String(), &harness_stream_rich);
		message_list->add_child(harness_stream_block);
		harness_streaming = true;
		harness_stream_text = String();
	}
	harness_stream_text += p_delta;
	if (harness_stream_rich) {
		harness_stream_rich->set_text(_markdown_to_bbcode(harness_stream_text));
	}
	// No direct scroll: growth fires _on_scrollbar_range_changed, which
	// follows only if the user is already at the bottom.
}

void AIStatusPanel::_reset_harness_stream() {
	if (!harness_streaming) {
		return;
	}
	// Abandon the stream block as-is (it holds whatever streamed) and put the
	// dots back for whatever the turn does next.
	harness_streaming = false;
	harness_stream_text = String();
	harness_stream_block = nullptr;
	harness_stream_rich = nullptr;
	if (!pending_message) {
		_show_pending_message();
	}
}

void AIStatusPanel::_cycle_policy_mode() {
	harness_policy_mode = (harness_policy_mode + 1) % 3;
	EditorSettings::get_singleton()->set_project_metadata("ai", "harness_policy_mode", harness_policy_mode);
	if (harness_driver.is_valid()) {
		harness_driver->set_policy_mode(harness_policy_mode);
	}
	_update_policy_mode_label();
}

void AIStatusPanel::_on_policy_mode_changed(int p_mode) {
	harness_policy_mode = CLAMP(p_mode, 0, 2);
	EditorSettings::get_singleton()->set_project_metadata("ai", "harness_policy_mode", harness_policy_mode);
	_update_policy_mode_label();
}

void AIStatusPanel::_update_policy_mode_label() {
	if (!policy_mode_label) {
		return;
	}
	switch (harness_policy_mode) {
		case CodexHarnessDriver::POLICY_AUTO:
			policy_mode_label->set_text(TTR("auto"));
			policy_mode_label->add_theme_color_override("font_color", AIColors::ACCENT_BLUE_MUTED);
			break;
		case CodexHarnessDriver::POLICY_PLAN:
			policy_mode_label->set_text(TTR("plan"));
			policy_mode_label->add_theme_color_override("font_color", Color(0.5f, 0.85f, 1.0f, 1.0f));
			break;
		case CodexHarnessDriver::POLICY_ASK:
		default:
			policy_mode_label->set_text(TTR("ask"));
			policy_mode_label->add_theme_color_override("font_color", AIColors::TEXT_MUTED);
			break;
	}
}

void AIStatusPanel::_build_approval_panel() {
	// Sits at the composer's slot; visibility-swapped with input_bar while an
	// approval is pending (modern-CLI style, transcript stays clean). Plain
	// text, no bordered block — it should read as the composer changing mode,
	// not as a chat element.
	MarginContainer *margin = memnew(MarginContainer);
	margin->add_theme_constant_override("margin_left", AIColors::PADDING_SM * EDSCALE);
	margin->add_theme_constant_override("margin_right", AIColors::PADDING_SM * EDSCALE);
	margin->add_theme_constant_override("margin_top", AIColors::PADDING_XS * EDSCALE);
	margin->add_theme_constant_override("margin_bottom", AIColors::PADDING_XS * EDSCALE);
	approval_panel = margin;

	VBoxContainer *vbox = memnew(VBoxContainer);
	vbox->add_theme_constant_override("separation", AIColors::PADDING_XS * EDSCALE);
	margin->add_child(vbox);

	approval_header = memnew(Label);
	approval_header->add_theme_color_override("font_color", AIColors::TEXT_PRIMARY);
	approval_header->add_theme_font_size_override("font_size", 12 * EDSCALE);
	vbox->add_child(approval_header);

	approval_body = memnew(RichTextLabel);
	approval_body->set_use_bbcode(false);
	approval_body->set_fit_content(true);
	approval_body->set_scroll_active(false);
	approval_body->set_selection_enabled(true);
	approval_body->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	approval_body->add_theme_color_override("default_color", AIColors::TEXT_SECONDARY);
	vbox->add_child(approval_body);

	HBoxContainer *buttons = memnew(HBoxContainer);
	buttons->add_theme_constant_override("separation", AIColors::PADDING_SM * EDSCALE);
	vbox->add_child(buttons);
	struct ButtonSpec {
		const char *label;
		const char *decision;
	};
	const ButtonSpec specs[] = {
		{ "Allow", "accept" },
		{ "Allow for session", "acceptForSession" },
		{ "Deny", "decline" },
	};
	for (const ButtonSpec &spec : specs) {
		Button *btn = memnew(Button);
		btn->set_text(TTR(spec.label));
		btn->connect(SceneStringNames::get_singleton()->pressed,
				callable_mp(this, &AIStatusPanel::_on_approval_decision).bind(String(spec.decision)));
		buttons->add_child(btn);
	}

	approval_panel->set_visible(false);
	add_child(approval_panel);
	move_child(approval_panel, input_bar->get_index());
}

void AIStatusPanel::_show_next_approval() {
	if (approval_queue.is_empty()) {
		if (approval_panel) {
			approval_panel->set_visible(false);
		}
		if (input_bar) {
			input_bar->set_visible(true);
		}
		if (prompt_edit) {
			prompt_edit->grab_focus();
		}
		return;
	}
	Dictionary info = approval_queue[0];
	String kind = info.get("kind", "command");
	if (kind == "file_change") {
		approval_header->set_text(TTR("Approval: apply file changes?"));
	} else if (kind == "exit_plan") {
		approval_header->set_text(TTR("Approve plan and execute?"));
	} else if (kind == "editor_tool") {
		approval_header->set_text(vformat(TTR("Approval: %s?"), String(info.get("tool", "action"))));
	} else {
		approval_header->set_text(TTR("Approval: run command?"));
	}
	String detail;
	if (kind == "file_change") {
		Array files = info.get("files", Array());
		for (int i = 0; i < files.size(); i++) {
			detail += String(files[i]) + "\n";
		}
	} else if (kind == "exit_plan") {
		detail = info.get("plan_summary", "");
	} else if (kind == "editor_tool") {
		Variant args = info.get("args", Dictionary());
		String args_json = args.get_type() == Variant::STRING ? String(args) : JSON::stringify(args);
		if (args_json != "{}" && !args_json.is_empty()) {
			detail = args_json;
		}
	} else {
		detail = info.get("command", "");
		String cwd = info.get("cwd", "");
		if (!cwd.is_empty()) {
			detail += "\n(in " + cwd + ")";
		}
	}
	String reason = info.get("reason", "");
	if (!reason.is_empty()) {
		detail += "\n" + reason;
	}
	if (detail.length() > 600) {
		detail = detail.substr(0, 600) + "\n[truncated]";
	}
	approval_body->set_text(detail.strip_edges());
	approval_body->set_visible(!detail.strip_edges().is_empty());

	if (input_bar) {
		input_bar->set_visible(false);
	}
	approval_panel->set_visible(true);
}

void AIStatusPanel::_on_harness_approval_requested(const Dictionary &p_info) {
	approval_queue.push_back(p_info);
	if (approval_panel && !approval_panel->is_visible()) {
		_show_next_approval();
	}
}

void AIStatusPanel::_on_approval_decision(const String &p_decision) {
	if (approval_queue.is_empty()) {
		return;
	}
	Dictionary info = approval_queue[0];
	approval_queue.remove_at(0);
	if (harness_driver.is_valid()) {
		harness_driver->respond_approval((int)info.get("request_id", -1), p_decision);
	}
	_show_next_approval();
}

void AIStatusPanel::_ensure_harness_driver() {
	if (harness_driver.is_valid()) {
		return;
	}
	harness_driver.instantiate();
	// The driver re-emits the orchestrator signal contract; reuse the
	// existing handlers wholesale.
	harness_driver->connect("run_started", callable_mp(this, &AIStatusPanel::_on_orchestrator_started));
	harness_driver->connect("api_round_started", callable_mp(this, &AIStatusPanel::_on_api_round_started));
	harness_driver->connect("progress_update", callable_mp(this, &AIStatusPanel::_on_orchestrator_progress));
	harness_driver->connect("assistant_item_ready", callable_mp(this, &AIStatusPanel::_on_orchestrator_assistant_item));
	harness_driver->connect("tool_result_ready", callable_mp(this, &AIStatusPanel::_on_orchestrator_tool_result));
	harness_driver->connect("turn_tokens_ready", callable_mp(this, &AIStatusPanel::_on_turn_tokens_ready));
	harness_driver->connect("run_complete", callable_mp(this, &AIStatusPanel::_on_orchestrator_complete));
	harness_driver->connect("checkpoint_recommended", callable_mp(this, &AIStatusPanel::_on_checkpoint_recommended));
	harness_driver->connect("assistant_delta", callable_mp(this, &AIStatusPanel::_on_harness_assistant_delta));
	harness_driver->connect("thinking_delta", callable_mp(this, &AIStatusPanel::_on_harness_thinking_delta));
	harness_driver->connect("thinking_done", callable_mp(this, &AIStatusPanel::_finalize_harness_thinking));
	harness_driver->connect("scene_diff_ready", callable_mp(this, &AIStatusPanel::_on_scene_diff_ready));
	harness_driver->connect("approval_requested", callable_mp(this, &AIStatusPanel::_on_harness_approval_requested));
	harness_driver->connect("policy_mode_changed", callable_mp(this, &AIStatusPanel::_on_policy_mode_changed));
	harness_driver->set_policy_mode(harness_policy_mode);
	harness_driver->set_model(harness_model);
	harness_driver->connect("todos_updated", callable_mp(this, &AIStatusPanel::_on_todos_updated));
	// Session continuity: resume this chat's codex thread if we have one.
	if (chat_store.is_valid()) {
		String saved_thread = EditorSettings::get_singleton()->get_project_metadata(
				"ai_harness_threads", chat_store->get_chat_id(), "");
		if (!saved_thread.is_empty()) {
			harness_driver->set_resume_thread_id(saved_thread);
		}
	}
	if (!harness_driver->start_session()) {
		ERR_PRINT("AI Chat Panel: failed to start codex harness session.");
		harness_driver.unref();
	}
}

void AIStatusPanel::_on_provider_changed(int p_index) {
	AI *ai = AI::get_singleton();
	if (!ai) {
		return;
	}

	String model_id = provider_dropdown->get_item_metadata(p_index);
	if (model_id.is_empty()) {
		return;
	}

	if (model_id == "codex-harness" || model_id.begins_with("codex-harness:")) {
		String new_model = model_id.get_slice(":", 1);
		if (new_model.is_empty()) {
			new_model = "kimi-k2.6"; // Bare "codex-harness" predates multi-model.
		}
		// A live driver keeps the model it started with, so a model change
		// needs a fresh driver. Chat continuity survives: the codex thread id
		// persists in ai_harness_threads and thread/resume re-applies the new
		// model override. (The driver can outlive harness mode — leaving it
		// doesn't shut it down — hence no use_harness_mode check here.)
		if (harness_driver.is_valid() && new_model != harness_model) {
			harness_driver->shutdown();
			harness_driver.unref();
			harness_streaming = false;
			harness_stream_text = String();
			harness_stream_block = nullptr;
			harness_stream_rich = nullptr;
			_finalize_harness_thinking();
		}
		use_harness_mode = true;
		harness_model = new_model;
		EditorSettings::get_singleton()->set_project_metadata("ai", "selected_model", model_id);
		print_line("AI: Switched to Codex Harness loop (" + harness_model + ")");
		return;
	}
	use_harness_mode = false;

	// Find the provider type for this model
	Vector<AIProvider::ModelEntry> models = AIProvider::get_available_models();
	String provider_name;
	for (int i = 0; i < models.size(); i++) {
		if (models[i].model_id == model_id) {
			provider_name = models[i].provider;
			break;
		}
	}

	Ref<AIProvider> new_provider;
	if (provider_name == "xai") {
		Ref<XAIProvider> p;
		p.instantiate();
		new_provider = p;
	} else if (provider_name == "openai") {
		Ref<OpenAIProvider> p;
		p.instantiate();
		new_provider = p;
	} else if (provider_name == "anthropic") {
		Ref<AnthropicProvider> p;
		p.instantiate();
		new_provider = p;
	} else if (provider_name == "gemini") {
		Ref<GeminiProvider> p;
		p.instantiate();
		new_provider = p;
	} else if (provider_name == "deepinfra") {
		Ref<DeepInfraProvider> p;
		p.instantiate();
		new_provider = p;
	} else if (provider_name == "parasail") {
		Ref<ParasailProvider> p;
		p.instantiate();
		new_provider = p;
	} else if (provider_name == "clarifai") {
		Ref<ClarifaiProvider> p;
		p.instantiate();
		new_provider = p;
	} else if (provider_name == "moonshot") {
		Ref<MoonshotProvider> p;
		p.instantiate();
		new_provider = p;
	}

	if (new_provider.is_valid()) {
		new_provider->set_model(model_id);
		ai->set_provider(new_provider);
		EditorSettings::get_singleton()->set_project_metadata("ai", "selected_model", model_id);
		print_line(vformat("AI: Switched to %s (%s)", new_provider->get_provider_name(), model_id));
	}
}

void AIStatusPanel::check_api_connectivity() {
	// Guard: skip if checks are already in progress
	if (pending_checks > 0) {
		print_verbose("AI Status: Connectivity check already in progress, skipping");
		return;
	}

	// Reset state
	openai_connected = false;
	gemini_connected = false;
	xai_connected = false;
	pending_checks = 3;

	if (status_indicator) {
		status_indicator->set_status(AIStatusIndicator::STATUS_CHECKING);
		status_indicator->set_tooltip_text(TTR("Checking API connectivity..."));
	}

	if (status_label) {
		status_label->set_text(TTR("Checking..."));
	}

	// Send requests to check each provider
	// OpenAI: GET https://api.openai.com/v1/models
	if (http_openai) {
		Error err = http_openai->request("https://api.openai.com/v1/models", PackedStringArray(), HTTPClient::METHOD_GET);
		if (err != OK) {
			pending_checks--;
			print_verbose("AI Status: Failed to send OpenAI check request");
		}
	}

	// Gemini: GET https://generativelanguage.googleapis.com/v1beta/models
	if (http_gemini) {
		Error err = http_gemini->request("https://generativelanguage.googleapis.com/v1beta/models", PackedStringArray(), HTTPClient::METHOD_GET);
		if (err != OK) {
			pending_checks--;
			print_verbose("AI Status: Failed to send Gemini check request");
		}
	}

	// x.ai: GET https://api.x.ai/v1/models
	if (http_xai) {
		Error err = http_xai->request("https://api.x.ai/v1/models", PackedStringArray(), HTTPClient::METHOD_GET);
		if (err != OK) {
			pending_checks--;
			print_verbose("AI Status: Failed to send x.ai check request");
		}
	}

	// If all requests failed to send, update status immediately
	if (pending_checks == 0) {
		_update_status_from_results();
	}
}

void AIStatusPanel::_on_openai_request_completed(int p_result, int p_code, const PackedStringArray &p_headers, const PackedByteArray &p_body) {
	// Consider connected if we got any HTTP response (even 401 means the server is reachable)
	if (p_result == HTTPRequest::RESULT_SUCCESS && p_code > 0) {
		openai_connected = true;
		print_verbose(vformat("AI Status: OpenAI endpoint reachable (HTTP %d)", p_code));
	} else {
		print_verbose(vformat("AI Status: OpenAI endpoint unreachable (result: %d)", p_result));
	}

	pending_checks--;
	if (pending_checks <= 0) {
		_update_status_from_results();
	}
}

void AIStatusPanel::_on_gemini_request_completed(int p_result, int p_code, const PackedStringArray &p_headers, const PackedByteArray &p_body) {
	if (p_result == HTTPRequest::RESULT_SUCCESS && p_code > 0) {
		gemini_connected = true;
		print_verbose(vformat("AI Status: Gemini endpoint reachable (HTTP %d)", p_code));
	} else {
		print_verbose(vformat("AI Status: Gemini endpoint unreachable (result: %d)", p_result));
	}

	pending_checks--;
	if (pending_checks <= 0) {
		_update_status_from_results();
	}
}

void AIStatusPanel::_on_xai_request_completed(int p_result, int p_code, const PackedStringArray &p_headers, const PackedByteArray &p_body) {
	if (p_result == HTTPRequest::RESULT_SUCCESS && p_code > 0) {
		xai_connected = true;
		print_verbose(vformat("AI Status: x.ai endpoint reachable (HTTP %d)", p_code));
	} else {
		print_verbose(vformat("AI Status: x.ai endpoint unreachable (result: %d)", p_result));
	}

	pending_checks--;
	if (pending_checks <= 0) {
		_update_status_from_results();
	}
}

void AIStatusPanel::_update_status_from_results() {
	bool any_connected = openai_connected || gemini_connected || xai_connected;

	if (status_indicator) {
		if (any_connected) {
			status_indicator->set_status(AIStatusIndicator::STATUS_CONNECTED);

			// Build tooltip showing which providers are available
			String tooltip = TTR("API Status: Connected") + "\n";
			if (openai_connected) {
				tooltip += "  • OpenAI: " + TTR("Reachable") + "\n";
			}
			if (gemini_connected) {
				tooltip += "  • Gemini: " + TTR("Reachable") + "\n";
			}
			if (xai_connected) {
				tooltip += "  • x.ai: " + TTR("Reachable") + "\n";
			}
			status_indicator->set_tooltip_text(tooltip.strip_edges());

			print_line("AI Status: Connected to at least one provider");
		} else {
			status_indicator->set_status(AIStatusIndicator::STATUS_DISCONNECTED);
			status_indicator->set_tooltip_text(TTR("API Status: No providers reachable\nCheck your internet connection."));
			print_line("AI Status: No providers reachable");
		}
	}

	if (status_label) {
		if (any_connected) {
			status_label->set_text(TTR("Connected"));
		} else {
			status_label->set_text(TTR("Disconnected"));
		}
	}
}

void AIStatusPanel::_update_context_usage(int p_used_chars, int p_max_chars) {
	if (!context_usage_label || p_max_chars <= 0) {
		return;
	}
	float fraction = (float)p_used_chars / (float)p_max_chars;
	int percent = CLAMP((int)(fraction * 100.0f), 0, 100);

	context_usage_label->set_text(vformat("| %d%%", percent));
	context_usage_label->set_tooltip_text(
		vformat(TTR("Context: %d / %d chars (~%d%%)"), p_used_chars, p_max_chars, percent));
	context_usage_label->set_visible(true);

	if (fraction >= 0.90f) {
		context_usage_label->add_theme_color_override("font_color", AIColors::ERROR);
	} else if (fraction >= 0.70f) {
		context_usage_label->add_theme_color_override("font_color", AIColors::WARNING);
	} else {
		context_usage_label->add_theme_color_override("font_color", AIColors::TEXT_MUTED);
	}
}

void AIStatusPanel::_reset_context_usage() {
	if (context_usage_label) {
		context_usage_label->set_visible(false);
	}
}

void AIStatusPanel::_refresh_context_usage() {
	if (!chat_store.is_valid()) {
		_reset_context_usage();
		return;
	}
	int max_context_chars = DEFAULT_MAX_CONTEXT_CHARS;
	if (Engine::get_singleton()->has_singleton("AI")) {
		Object *ai_obj = Engine::get_singleton()->get_singleton_object("AI");
		AI *ai_inst = Object::cast_to<AI>(ai_obj);
		if (ai_inst && ai_inst->get_provider().is_valid()) {
			int window_tokens = ai_inst->get_provider()->get_context_window_tokens();
			if (window_tokens > 0) {
				max_context_chars = (window_tokens - 10000) * 4;
			}
		}
	}
	int total_chars = 0;
	const Vector<HistoryItem> &items = chat_store->get_items();
	for (int i = 0; i < items.size(); i++) {
		if (!items[i].is_injection()) {
			total_chars += _item_char_count(items[i]);
		}
	}
	if (total_chars == 0) {
		_reset_context_usage();
		return;
	}
	_update_context_usage(MIN(total_chars, max_context_chars), max_context_chars);
}

AIStatusPanel::AIStatusPanel() {
	set_name("AI");

	// Apply dark background to main panel
	Ref<StyleBoxFlat> panel_bg;
	panel_bg.instantiate();
	panel_bg->set_bg_color(AIColors::BG_0);
	panel_bg->set_content_margin(SIDE_LEFT, AIColors::PADDING_SM * EDSCALE);
	panel_bg->set_content_margin(SIDE_RIGHT, AIColors::PADDING_SM * EDSCALE);
	panel_bg->set_content_margin(SIDE_TOP, AIColors::PADDING_SM * EDSCALE);
	panel_bg->set_content_margin(SIDE_BOTTOM, AIColors::PADDING_XS * EDSCALE); // Minimal bottom padding
	add_theme_style_override("panel", panel_bg);

	// Initialize chat store
	chat_store.instantiate();

	// ========================================
	// Chat toolbar (very top) - "+ New" and history
	// ========================================
	chat_toolbar = memnew(HBoxContainer);
	chat_toolbar->add_theme_constant_override("separation", AIColors::PADDING_XS * EDSCALE);
	add_child(chat_toolbar);

	// "AI" label
	Label *ai_label = memnew(Label);
	ai_label->set_text(TTR("AI"));
	ai_label->add_theme_color_override("font_color", AIColors::TEXT_SECONDARY);
	chat_toolbar->add_child(ai_label);

	// Spacer
	Control *toolbar_spacer = memnew(Control);
	toolbar_spacer->set_h_size_flags(SIZE_EXPAND_FILL);
	chat_toolbar->add_child(toolbar_spacer);

	// Token count toggle button
	token_toggle_button = memnew(Button);
	token_toggle_button->set_text(TTR("Debug"));
	token_toggle_button->set_flat(true);
	token_toggle_button->set_toggle_mode(true);
	token_toggle_button->set_tooltip_text(TTR("Toggle debug info: token counts on tool results, latency on responses"));
	token_toggle_button->set_default_cursor_shape(Control::CURSOR_POINTING_HAND);
	token_toggle_button->add_theme_color_override("font_color", AIColors::TEXT_SECONDARY);
	token_toggle_button->connect(SceneStringNames::get_singleton()->pressed, callable_mp(this, &AIStatusPanel::_on_token_toggle_pressed));
	chat_toolbar->add_child(token_toggle_button);

	// History button
	history_button = memnew(Button);
	history_button->set_text(TTR("History"));
	history_button->set_flat(true);
	history_button->set_default_cursor_shape(Control::CURSOR_POINTING_HAND);
	history_button->add_theme_color_override("font_color", AIColors::TEXT_SECONDARY);
	history_button->connect(SceneStringNames::get_singleton()->pressed, callable_mp(this, &AIStatusPanel::_show_history_popup));
	chat_toolbar->add_child(history_button);

	// New chat button
	new_chat_button = memnew(Button);
	new_chat_button->set_text(TTR("+"));
	new_chat_button->set_flat(true);
	new_chat_button->set_tooltip_text(TTR("Start a new chat"));
	new_chat_button->set_default_cursor_shape(Control::CURSOR_POINTING_HAND);
	new_chat_button->add_theme_color_override("font_color", AIColors::TEXT_SECONDARY);
	new_chat_button->connect(SceneStringNames::get_singleton()->pressed, callable_mp(this, &AIStatusPanel::_new_chat));
	chat_toolbar->add_child(new_chat_button);

	// ========================================
	// History popup panel (no scroll — fixed width, auto height)
	// ========================================
	history_popup = memnew(PopupPanel);
	add_child(history_popup);

	history_list = memnew(VBoxContainer);
	history_list->set_h_size_flags(SIZE_EXPAND_FILL);
	history_list->add_theme_constant_override("separation", 2 * EDSCALE);
	history_popup->add_child(history_list);

	// Delete confirmation dialog
	delete_chat_dialog = memnew(ConfirmationDialog);
	delete_chat_dialog->set_text(TTR("Delete this chat? This cannot be undone."));
	delete_chat_dialog->get_ok_button()->set_text(TTR("Delete"));
	delete_chat_dialog->connect("confirmed", callable_mp(this, &AIStatusPanel::_on_delete_chat_confirmed));
	add_child(delete_chat_dialog);

	// ========================================
	// Transcript scroll area (top, expandable)
	// ========================================
	transcript_scroll = memnew(ScrollContainer);
	transcript_scroll->set_h_size_flags(SIZE_EXPAND_FILL);
	transcript_scroll->set_v_size_flags(SIZE_EXPAND_FILL);
	transcript_scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	add_child(transcript_scroll);

	// Dark background for scroll container
	Ref<StyleBoxFlat> scroll_style;
	scroll_style.instantiate();
	scroll_style->set_bg_color(AIColors::BG_0);
	scroll_style->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
	transcript_scroll->add_theme_style_override("panel", scroll_style);

	message_list = memnew(VBoxContainer);
	message_list->set_h_size_flags(SIZE_EXPAND_FILL);
	message_list->add_theme_constant_override("separation", AIColors::PADDING_SM * EDSCALE);
	transcript_scroll->add_child(message_list);

	// ========================================
	// Separator - subtle divider line (compact)
	// ========================================
	HSeparator *separator = memnew(HSeparator);
	Ref<StyleBoxFlat> sep_style;
	sep_style.instantiate();
	sep_style->set_bg_color(AIColors::BORDER);
	sep_style->set_content_margin(SIDE_TOP, 2 * EDSCALE);
	sep_style->set_content_margin(SIDE_BOTTOM, 2 * EDSCALE);
	separator->add_theme_style_override("separator", sep_style);
	add_child(separator);

	// ========================================
	// Todo panel (AI's self-managed task list)
	// ========================================
	todo_panel = memnew(AITodoPanelWidget);
	todo_panel->set_visible(false);
	add_child(todo_panel);

	// ========================================
	// Queue display (simple list above input)
	// ========================================
	queue_container = memnew(VBoxContainer);
	queue_container->set_visible(false); // Hidden when empty
	queue_container->add_theme_constant_override("separation", AIColors::PADDING_XS * EDSCALE);
	add_child(queue_container);

	// Queue header label ("Queued")
	queue_header_label = memnew(Label);
	queue_header_label->set_text(TTR("Queued"));
	queue_header_label->add_theme_color_override("font_color", AIColors::TEXT_SECONDARY);
	queue_container->add_child(queue_header_label);

	// ========================================
	// Parse error pill (hidden until script actions have parse errors)
	// ========================================
	parse_error_pill = memnew(ParseErrorPill);
	parse_error_pill->add_theme_constant_override("margin_left", AIColors::PADDING_SM * EDSCALE);
	parse_error_pill->add_theme_constant_override("margin_right", AIColors::PADDING_SM * EDSCALE);
	parse_error_pill->add_theme_constant_override("margin_bottom", AIColors::PADDING_XS * EDSCALE);
	add_child(parse_error_pill);

	// ========================================
	// Debug context pill (hidden until game has errors or is running)
	// =========================================================
	debug_pill = memnew(DebugContextPill);
	debug_pill->add_theme_constant_override("margin_left", AIColors::PADDING_SM * EDSCALE);
	debug_pill->add_theme_constant_override("margin_right", AIColors::PADDING_SM * EDSCALE);
	debug_pill->add_theme_constant_override("margin_bottom", AIColors::PADDING_XS * EDSCALE);
	debug_pill->connect("enabled_changed", callable_mp(this, &AIStatusPanel::_on_debug_context_toggled));
	add_child(debug_pill);

	// Poll game state every second to update the pill (catches F5 press)
	debug_pill_update_timer = memnew(Timer);
	debug_pill_update_timer->set_wait_time(1.0);
	debug_pill_update_timer->set_one_shot(false);
	debug_pill_update_timer->connect("timeout", callable_mp(this, &AIStatusPanel::_on_debug_pill_update_tick));
	add_child(debug_pill_update_timer);
	// start() is deferred to NOTIFICATION_READY so the timer is in the scene tree

	// Shared right-click context menu for message bubbles
	bubble_context_menu = memnew(PopupMenu);
	bubble_context_menu->add_item(TTR("Copy text"), 0);
	bubble_context_menu->connect("id_pressed", callable_mp(this, &AIStatusPanel::_on_bubble_menu_id_pressed));
	add_child(bubble_context_menu);

	// Image preview strip (hidden when empty)
	// ========================================
	image_preview_strip = memnew(HBoxContainer);
	image_preview_strip->add_theme_constant_override("separation", AIColors::PADDING_XS * EDSCALE);
	image_preview_strip->set_visible(false);
	add_child(image_preview_strip);

	// ========================================
	// Drag handle for resizing input box
	// ========================================
	input_drag_handle = memnew(Control);
	input_drag_handle->set_custom_minimum_size(Size2(0, 6 * EDSCALE));
	input_drag_handle->set_default_cursor_shape(Control::CURSOR_VSIZE);
	input_drag_handle->connect("gui_input", callable_mp(this, &AIStatusPanel::_on_drag_handle_gui_input));
	add_child(input_drag_handle);

	// ========================================
	// Input bar (bottom)
	// ========================================
	input_bar = memnew(HBoxContainer);
	input_bar->add_theme_constant_override("separation", AIColors::PADDING_SM * EDSCALE);
	add_child(input_bar);
	_build_approval_panel();

	// Prompt text edit
	prompt_edit = memnew(TextEdit);
	prompt_edit->set_placeholder(TTR("Type a message..."));
	prompt_edit->set_h_size_flags(SIZE_EXPAND_FILL);
	prompt_edit->set_custom_minimum_size(Size2(0, 60 * EDSCALE));
	prompt_edit->set_line_wrapping_mode(TextEdit::LINE_WRAPPING_BOUNDARY);
	// Line spacing while composing, matching the bubble's paragraph rhythm.
	prompt_edit->add_theme_constant_override("line_spacing", (int)(8 * EDSCALE));
	prompt_edit->connect("text_changed", callable_mp(this, &AIStatusPanel::_on_prompt_text_changed));
	prompt_edit->connect("gui_input", callable_mp(this, &AIStatusPanel::_on_prompt_gui_input));
	// Forward drag-and-drop from the FileSystem dock onto the input — drops the
	// dragged file paths in at the caret. The TextEdit itself doesn't accept drops
	// by default; we inherit its handling via the panel.
	SET_DRAG_FORWARDING_CDU(prompt_edit, AIStatusPanel);

	// Dark input field styling - normal state
	Ref<StyleBoxFlat> prompt_normal;
	prompt_normal.instantiate();
	prompt_normal->set_bg_color(AIColors::BG_2);
	prompt_normal->set_border_width_all(1);
	prompt_normal->set_border_color(AIColors::BORDER);
	prompt_normal->set_corner_radius_all(AIColors::CORNER_RADIUS_MD * EDSCALE);
	prompt_normal->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
	prompt_edit->add_theme_style_override("normal", prompt_normal);

	// Focus state - highlighted border
	Ref<StyleBoxFlat> prompt_focus;
	prompt_focus.instantiate();
	prompt_focus->set_bg_color(AIColors::BG_2);
	prompt_focus->set_border_width_all(2);
	prompt_focus->set_border_color(AIColors::ACCENT_BLUE);
	prompt_focus->set_corner_radius_all(AIColors::CORNER_RADIUS_MD * EDSCALE);
	prompt_focus->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
	prompt_edit->add_theme_style_override("focus", prompt_focus);

	prompt_edit->add_theme_color_override("font_color", AIColors::TEXT_PRIMARY);
	prompt_edit->add_theme_color_override("font_placeholder_color", AIColors::TEXT_MUTED);
	prompt_edit->add_theme_color_override("caret_color", AIColors::ACCENT_BLUE);
	prompt_edit->add_theme_color_override("selection_color", AIColors::ACCENT_BLUE_MUTED);
	prompt_edit->add_theme_constant_override("line_spacing", 2);

	input_bar->add_child(prompt_edit);

	// Button column
	VBoxContainer *button_column = memnew(VBoxContainer);
	button_column->add_theme_constant_override("separation", AIColors::PADDING_XS * EDSCALE);
	input_bar->add_child(button_column);

	// === Send/Stop button (toggles based on run state) ===
	send_button = memnew(Button);
	send_button->set_text(TTR("Send"));
	send_button->set_disabled(true);
	send_button->connect(SceneStringNames::get_singleton()->pressed, callable_mp(this, &AIStatusPanel::_on_send_button_pressed));

	// Send button - normal state
	Ref<StyleBoxFlat> send_normal;
	send_normal.instantiate();
	send_normal->set_bg_color(AIColors::ACCENT_BLUE);
	send_normal->set_corner_radius_all(AIColors::CORNER_RADIUS_MD * EDSCALE);
	send_normal->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
	send_button->add_theme_style_override("normal", send_normal);

	// Send button - hover state
	Ref<StyleBoxFlat> send_hover;
	send_hover.instantiate();
	send_hover->set_bg_color(AIColors::ACCENT_BLUE_HOVER);
	send_hover->set_corner_radius_all(AIColors::CORNER_RADIUS_MD * EDSCALE);
	send_hover->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
	send_button->add_theme_style_override("hover", send_hover);

	// Send button - pressed state
	Ref<StyleBoxFlat> send_pressed;
	send_pressed.instantiate();
	send_pressed->set_bg_color(AIColors::ACCENT_BLUE_PRESSED);
	send_pressed->set_corner_radius_all(AIColors::CORNER_RADIUS_MD * EDSCALE);
	send_pressed->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
	send_button->add_theme_style_override("pressed", send_pressed);

	// Send button - disabled state
	Ref<StyleBoxFlat> send_disabled;
	send_disabled.instantiate();
	send_disabled->set_bg_color(AIColors::BG_2);
	send_disabled->set_corner_radius_all(AIColors::CORNER_RADIUS_MD * EDSCALE);
	send_disabled->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
	send_button->add_theme_style_override("disabled", send_disabled);

	send_button->add_theme_color_override("font_color", AIColors::TEXT_PRIMARY);
	send_button->add_theme_color_override("font_hover_color", AIColors::TEXT_PRIMARY);
	send_button->add_theme_color_override("font_pressed_color", AIColors::TEXT_PRIMARY);
	send_button->add_theme_color_override("font_disabled_color", AIColors::TEXT_DISABLED);

	button_column->add_child(send_button);


	// ========================================
	// Status bar (bottom) - compact and subtle
	// ========================================
	HBoxContainer *status_bar = memnew(HBoxContainer);
	status_bar->add_theme_constant_override("separation", 4 * EDSCALE); // Tight spacing between indicator and label
	status_bar->set_v_size_flags(SIZE_SHRINK_CENTER); // Don't expand vertically
	add_child(status_bar);

	// Status indicator (colored circle)
	status_indicator = memnew(AIStatusIndicator);
	status_indicator->set_tooltip_text(TTR("API connection status"));
	status_bar->add_child(status_indicator);

	// Status label - muted appearance
	status_label = memnew(Label);
	status_label->set_text(TTR("Unknown"));
	status_label->add_theme_color_override("font_color", AIColors::TEXT_MUTED);
	status_bar->add_child(status_label);

	// Model dropdown
	provider_dropdown = memnew(OptionButton);
	provider_dropdown->add_theme_font_size_override("font_size", 11 * EDSCALE);
	Vector<AIProvider::ModelEntry> models = AIProvider::get_available_models();
	String saved_model = EditorSettings::get_singleton()->get_project_metadata("ai", "selected_model", "grok-4-fast");
	int default_idx = 0;
	for (int i = 0; i < models.size(); i++) {
		provider_dropdown->add_item(models[i].display_name, i);
		provider_dropdown->set_item_metadata(i, models[i].model_id);
		if (models[i].model_id == saved_model) {
			default_idx = i;
		}
	}
	// Experimental codex-harness loop (Phase 2 of the harness replacement).
	// Metadata is "codex-harness" (bare = K2.6, kept for saved-setting compat)
	// or "codex-harness:<model>" for other translator-routed models.
	{
		int harness_idx = models.size();
		provider_dropdown->add_item("Kimi K2.6 (Codex Harness)", harness_idx);
		provider_dropdown->set_item_metadata(harness_idx, "codex-harness");
		if (saved_model == "codex-harness") {
			default_idx = harness_idx;
		}
		int harness_k3_idx = harness_idx + 1;
		provider_dropdown->add_item("Kimi K3 (Codex Harness)", harness_k3_idx);
		provider_dropdown->set_item_metadata(harness_k3_idx, "codex-harness:kimi-k3");
		if (saved_model == "codex-harness:kimi-k3") {
			default_idx = harness_k3_idx;
		}
	}
	provider_dropdown->select(default_idx);
	provider_dropdown->set_tooltip_text(TTR("Switch AI model"));
	provider_dropdown->connect("item_selected", callable_mp(this, &AIStatusPanel::_on_provider_changed));
	PopupMenu *popup = provider_dropdown->get_popup();
	for (int i = 0; i < popup->get_item_count(); i++) {
		popup->set_item_as_radio_checkable(i, false);
	}
	status_bar->add_child(provider_dropdown);

	// Approval mode indicator (harness loop; Shift+Tab in the composer cycles)
	harness_policy_mode = (int)EditorSettings::get_singleton()->get_project_metadata("ai", "harness_policy_mode", 0);
	policy_mode_label = memnew(Label);
	policy_mode_label->add_theme_font_size_override("font_size", 11 * EDSCALE);
	policy_mode_label->set_tooltip_text(TTR("Approval mode for agent commands and file edits.\nShift+Tab in the composer to cycle."));
	status_bar->add_child(policy_mode_label);
	_update_policy_mode_label();

	// Apply saved model selection to the AI singleton
	_on_provider_changed(default_idx);

	// Context usage label - right side of status bar, hidden until first run
	context_usage_label = memnew(Label);
	context_usage_label->add_theme_font_size_override("font_size", 11 * EDSCALE);
	context_usage_label->add_theme_color_override("font_color", AIColors::TEXT_MUTED);
	context_usage_label->set_visible(false);
	status_bar->add_child(context_usage_label);

	// ========================================
	// HTTP request nodes for connectivity checks
	// ========================================
	http_openai = memnew(HTTPRequest);
	http_openai->set_timeout(10.0); // 10 second timeout
	add_child(http_openai);
	http_openai->connect("request_completed", callable_mp(this, &AIStatusPanel::_on_openai_request_completed));

	http_gemini = memnew(HTTPRequest);
	http_gemini->set_timeout(10.0);
	add_child(http_gemini);
	http_gemini->connect("request_completed", callable_mp(this, &AIStatusPanel::_on_gemini_request_completed));

	http_xai = memnew(HTTPRequest);
	http_xai->set_timeout(10.0);
	add_child(http_xai);
	http_xai->connect("request_completed", callable_mp(this, &AIStatusPanel::_on_xai_request_completed));

	// ========================================
	// Rewind/Edit confirmation dialog (custom three-button layout)
	// ========================================
	rewind_dialog = memnew(AcceptDialog);
	rewind_dialog->set_title(TTR("Rewind conversation?"));
	rewind_dialog->set_exclusive(false);
	rewind_dialog->get_ok_button()->hide(); // We'll use custom buttons
	rewind_dialog->set_flag(Window::FLAG_RESIZE_DISABLED, true); // Prevent resizing
	add_child(rewind_dialog);

	// Main content VBox with fixed width to prevent stretching
	VBoxContainer *dialog_vbox = memnew(VBoxContainer);
	dialog_vbox->add_theme_constant_override("separation", AIColors::PADDING_SM * EDSCALE);
	dialog_vbox->set_custom_minimum_size(Size2(400 * EDSCALE, 0)); // Fixed width
	rewind_dialog->add_child(dialog_vbox);

	// Explanation label (stored to update for edit/rewind modes)
	rewind_dialog_label = memnew(Label);
	rewind_dialog_label->set_text(TTR("Rewinding will clear the messages after this one and let you continue from here."));
	rewind_dialog_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	rewind_dialog_label->set_custom_minimum_size(Size2(380 * EDSCALE, 0)); // Width for wrapping
	rewind_dialog_label->add_theme_color_override("font_color", AIColors::TEXT_SECONDARY);
	dialog_vbox->add_child(rewind_dialog_label);

	// "Don't ask again" checkbox
	dont_ask_again_checkbox = memnew(CheckBox);
	dont_ask_again_checkbox->set_text(TTR("Don't ask again"));
	dont_ask_again_checkbox->add_theme_color_override("font_color", AIColors::TEXT_MUTED);
	dialog_vbox->add_child(dont_ask_again_checkbox);

	// Button row
	HBoxContainer *button_row = memnew(HBoxContainer);
	button_row->add_theme_constant_override("separation", AIColors::PADDING_SM * EDSCALE);
	dialog_vbox->add_child(button_row);

	// Cancel button
	cancel_button = memnew(Button);
	cancel_button->set_text(TTR("Cancel (esc)"));
	cancel_button->connect(SceneStringNames::get_singleton()->pressed, callable_mp(this, &AIStatusPanel::_on_dialog_cancel));
	cancel_button->add_theme_color_override("font_color", AIColors::TEXT_SECONDARY);
	cancel_button->add_theme_color_override("font_hover_color", AIColors::TEXT_PRIMARY);
	button_row->add_child(cancel_button);

	// Continue without reverting button (outlined style)
	continue_no_revert_button = memnew(Button);
	continue_no_revert_button->set_text(TTR("Continue without reverting"));
	continue_no_revert_button->connect(SceneStringNames::get_singleton()->pressed, callable_mp(this, &AIStatusPanel::_on_dialog_continue_no_revert));

	Ref<StyleBoxFlat> outline_style;
	outline_style.instantiate();
	outline_style->set_bg_color(Color(0, 0, 0, 0)); // Transparent
	outline_style->set_border_width_all(1);
	outline_style->set_border_color(AIColors::BORDER_LIGHT);
	outline_style->set_corner_radius_all(AIColors::CORNER_RADIUS_MD * EDSCALE);
	outline_style->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
	continue_no_revert_button->add_theme_style_override("normal", outline_style);

	Ref<StyleBoxFlat> outline_hover;
	outline_hover.instantiate();
	outline_hover->set_bg_color(AIColors::BG_3);
	outline_hover->set_border_width_all(1);
	outline_hover->set_border_color(AIColors::BORDER_LIGHT);
	outline_hover->set_corner_radius_all(AIColors::CORNER_RADIUS_MD * EDSCALE);
	outline_hover->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
	continue_no_revert_button->add_theme_style_override("hover", outline_hover);

	continue_no_revert_button->add_theme_color_override("font_color", AIColors::TEXT_SECONDARY);
	continue_no_revert_button->add_theme_color_override("font_hover_color", AIColors::TEXT_PRIMARY);
	button_row->add_child(continue_no_revert_button);

	// Continue and revert button (filled accent style)
	continue_revert_button = memnew(Button);
	continue_revert_button->set_text(TTR("Continue and revert"));
	continue_revert_button->connect(SceneStringNames::get_singleton()->pressed, callable_mp(this, &AIStatusPanel::_on_dialog_continue_revert));

	Ref<StyleBoxFlat> accent_style;
	accent_style.instantiate();
	accent_style->set_bg_color(AIColors::ACCENT_BLUE);
	accent_style->set_corner_radius_all(AIColors::CORNER_RADIUS_MD * EDSCALE);
	accent_style->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
	continue_revert_button->add_theme_style_override("normal", accent_style);

	Ref<StyleBoxFlat> accent_hover;
	accent_hover.instantiate();
	accent_hover->set_bg_color(AIColors::ACCENT_BLUE_HOVER);
	accent_hover->set_corner_radius_all(AIColors::CORNER_RADIUS_MD * EDSCALE);
	accent_hover->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
	continue_revert_button->add_theme_style_override("hover", accent_hover);

	Ref<StyleBoxFlat> accent_disabled;
	accent_disabled.instantiate();
	accent_disabled->set_bg_color(AIColors::BG_2);
	accent_disabled->set_corner_radius_all(AIColors::CORNER_RADIUS_MD * EDSCALE);
	accent_disabled->set_content_margin_all(AIColors::PADDING_SM * EDSCALE);
	continue_revert_button->add_theme_style_override("disabled", accent_disabled);

	continue_revert_button->add_theme_color_override("font_color", AIColors::TEXT_PRIMARY);
	continue_revert_button->add_theme_color_override("font_hover_color", AIColors::TEXT_PRIMARY);
	continue_revert_button->add_theme_color_override("font_disabled_color", AIColors::TEXT_DISABLED);
	button_row->add_child(continue_revert_button);

	// ========================================
	// Image lightbox popup (handles 1..N images via AIImageViewer)
	// ========================================
	image_viewer = memnew(AIImageViewer);
	add_child(image_viewer);

	// Thinking dot animation timer
	thinking_dot_timer = memnew(Timer);
	thinking_dot_timer->set_wait_time(0.4);
	thinking_dot_timer->set_one_shot(false);
	thinking_dot_timer->connect("timeout", callable_mp(this, &AIStatusPanel::_on_thinking_dot_tick));
	add_child(thinking_dot_timer);

	// Pending tool card spinner — shared across all in-flight tool cards.
	pending_tool_timer = memnew(Timer);
	pending_tool_timer->set_wait_time(0.1);
	pending_tool_timer->set_one_shot(false);
	pending_tool_timer->connect("timeout", callable_mp(this, &AIStatusPanel::_on_pending_tool_spinner_tick));
	add_child(pending_tool_timer);
}

AIStatusPanel::~AIStatusPanel() {
	// Disconnect from AI provider signal if connected
	if (Engine::get_singleton()->has_singleton("AI")) {
		Object *ai_obj = Engine::get_singleton()->get_singleton_object("AI");
		AI *ai = Object::cast_to<AI>(ai_obj);
		if (ai) {
			Ref<AIProvider> provider = ai->get_provider();
			if (provider.is_valid()) {
				if (provider->is_connected("request_completed", callable_mp(this, &AIStatusPanel::_on_ai_response))) {
					provider->disconnect("request_completed", callable_mp(this, &AIStatusPanel::_on_ai_response));
				}
			}

			// Disconnect from orchestrator signals if connected
			Ref<AgenticOrchestrator> orchestrator = ai->get_orchestrator();
			if (orchestrator.is_valid()) {
				if (orchestrator->is_connected("run_started", callable_mp(this, &AIStatusPanel::_on_orchestrator_started))) {
					orchestrator->disconnect("run_started", callable_mp(this, &AIStatusPanel::_on_orchestrator_started));
				}
				if (orchestrator->is_connected("progress_update", callable_mp(this, &AIStatusPanel::_on_orchestrator_progress))) {
					orchestrator->disconnect("progress_update", callable_mp(this, &AIStatusPanel::_on_orchestrator_progress));
				}
				if (orchestrator->is_connected("tool_result_ready", callable_mp(this, &AIStatusPanel::_on_orchestrator_tool_result))) {
					orchestrator->disconnect("tool_result_ready", callable_mp(this, &AIStatusPanel::_on_orchestrator_tool_result));
				}
				if (orchestrator->is_connected("run_complete", callable_mp(this, &AIStatusPanel::_on_orchestrator_complete))) {
					orchestrator->disconnect("run_complete", callable_mp(this, &AIStatusPanel::_on_orchestrator_complete));
				}
				if (orchestrator->is_connected("checkpoint_recommended", callable_mp(this, &AIStatusPanel::_on_checkpoint_recommended))) {
					orchestrator->disconnect("checkpoint_recommended", callable_mp(this, &AIStatusPanel::_on_checkpoint_recommended));
				}
				if (orchestrator->is_connected("assistant_item_ready", callable_mp(this, &AIStatusPanel::_on_orchestrator_assistant_item))) {
					orchestrator->disconnect("assistant_item_ready", callable_mp(this, &AIStatusPanel::_on_orchestrator_assistant_item));
				}
				if (orchestrator->is_connected("todos_updated", callable_mp(this, &AIStatusPanel::_on_todos_updated))) {
					orchestrator->disconnect("todos_updated", callable_mp(this, &AIStatusPanel::_on_todos_updated));
				}
				if (orchestrator->is_connected("scene_diff_ready", callable_mp(this, &AIStatusPanel::_on_scene_diff_ready))) {
					orchestrator->disconnect("scene_diff_ready", callable_mp(this, &AIStatusPanel::_on_scene_diff_ready));
				}
				if (orchestrator->is_connected("user_injection_consumed", callable_mp(this, &AIStatusPanel::_on_user_injection_consumed))) {
					orchestrator->disconnect("user_injection_consumed", callable_mp(this, &AIStatusPanel::_on_user_injection_consumed));
				}
				if (orchestrator->is_connected("api_round_started", callable_mp(this, &AIStatusPanel::_on_api_round_started))) {
					orchestrator->disconnect("api_round_started", callable_mp(this, &AIStatusPanel::_on_api_round_started));
				}
			}
		}
	}
}

// ============================================================================
// AIStatusIndicatorPlugin - The EditorPlugin that manages everything
// ============================================================================

void AIStatusIndicatorPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			// Create the panel and add it to dock (right side, upper area)
			panel = memnew(AIStatusPanel);
			add_control_to_dock(DOCK_SLOT_RIGHT_UL, panel);

			// Connect AI screenshot signal → panel's _add_pending_image
			if (Engine::get_singleton()->has_singleton("AI")) {
				Object *ai_obj = Engine::get_singleton()->get_singleton_object("AI");
				if (ai_obj && panel) {
					ai_obj->connect("screenshot_for_chat",
							callable_mp(panel, &AIStatusPanel::_add_pending_image));
				}
			}

			// Create fallback timer (5 minutes = 300 seconds)
			fallback_timer = memnew(Timer);
			fallback_timer->set_wait_time(300.0);
			fallback_timer->set_one_shot(false);
			fallback_timer->connect("timeout", callable_mp(this, &AIStatusIndicatorPlugin::_on_fallback_timer_timeout));
			add_child(fallback_timer);
			fallback_timer->start();
		} break;

		case NOTIFICATION_EXIT_TREE: {
			if (fallback_timer) {
				fallback_timer->stop();
				remove_child(fallback_timer);
				memdelete(fallback_timer);
				fallback_timer = nullptr;
			}

			// Disconnect AI screenshot signal
			if (Engine::get_singleton()->has_singleton("AI")) {
				Object *ai_obj = Engine::get_singleton()->get_singleton_object("AI");
				if (ai_obj && panel && ai_obj->is_connected("screenshot_for_chat",
						callable_mp(panel, &AIStatusPanel::_add_pending_image))) {
					ai_obj->disconnect("screenshot_for_chat",
							callable_mp(panel, &AIStatusPanel::_add_pending_image));
				}
			}

			if (panel) {
				remove_control_from_docks(panel);
				memdelete(panel);
				panel = nullptr;
			}
		} break;

		case NOTIFICATION_APPLICATION_FOCUS_IN: {
			// Check connectivity when editor regains focus
			if (panel) {
				print_verbose("AI Status: Editor focus regained, checking connectivity...");
				panel->check_api_connectivity();
			}
		} break;
	}
}

void AIStatusIndicatorPlugin::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_on_fallback_timer_timeout"), &AIStatusIndicatorPlugin::_on_fallback_timer_timeout);
}

void AIStatusIndicatorPlugin::_on_fallback_timer_timeout() {
	// Periodic connectivity check (fallback)
	if (panel) {
		print_verbose("AI Status: Periodic connectivity check...");
		panel->check_api_connectivity();
	}
}

AIStatusIndicatorPlugin::AIStatusIndicatorPlugin() {
}

AIStatusIndicatorPlugin::~AIStatusIndicatorPlugin() {
}
