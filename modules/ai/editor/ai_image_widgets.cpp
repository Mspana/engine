/**************************************************************************/
/*  ai_image_widgets.cpp                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "ai_image_widgets.h"

#include "core/core_bind.h"
#include "core/io/image.h"
#include "core/io/marshalls.h"
#include "core/math/math_funcs.h"
#include "editor/themes/editor_scale.h"
#include "scene/main/viewport.h"
#include "scene/resources/style_box_flat.h"

// ============================================================================
// AIImageViewer
// ============================================================================

void AIImageViewer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("popup_for_images", "b64s", "start_index"), &AIImageViewer::popup_for_images, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("set_image_index", "index"), &AIImageViewer::set_image_index);
	ClassDB::bind_method(D_METHOD("get_image_index"), &AIImageViewer::get_image_index);
	ClassDB::bind_method(D_METHOD("get_image_count"), &AIImageViewer::get_image_count);
}

AIImageViewer::AIImageViewer() {
	set_flag(Window::FLAG_POPUP, true);
	set_flag(Window::FLAG_RESIZE_DISABLED, true);

	// Replace the editor's default popup stylebox (which shows a chunky border)
	// with a clean dark panel so the image is the visual focus.
	Ref<StyleBoxFlat> panel_style;
	panel_style.instantiate();
	panel_style->set_bg_color(Color(0.06f, 0.06f, 0.07f, 1.0f));
	panel_style->set_border_width_all(0);
	panel_style->set_corner_radius_all(0);
	add_theme_style_override("panel", panel_style);

	_root = memnew(Control);
	_root->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	_root->set_clip_contents(true); // So the offscreen layer doesn't bleed into the popup edges during animation.
	_root->set_mouse_filter(Control::MOUSE_FILTER_PASS);
	add_child(_root);

	auto build_main_layer = [this]() -> TextureRect * {
		TextureRect *tr = memnew(TextureRect);
		tr->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
		tr->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
		tr->set_expand_mode(TextureRect::EXPAND_FIT_WIDTH_PROPORTIONAL);
		tr->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		_root->add_child(tr);
		return tr;
	};
	_main_a = build_main_layer();
	_main_b = build_main_layer();
	_main_b->hide();

	auto build_arrow = [this](const String &p_label, int p_direction) -> Button * {
		Button *btn = memnew(Button);
		btn->set_text(p_label);
		btn->set_focus_mode(Control::FOCUS_NONE); // Keep arrow keys flowing to window_input.
		btn->set_mouse_filter(Control::MOUSE_FILTER_STOP);
		btn->set_default_cursor_shape(Control::CURSOR_POINTING_HAND);
		btn->set_custom_minimum_size(Size2(36, 60) * EDSCALE);
		btn->add_theme_font_size_override("font_size", int(22 * EDSCALE));

		// Soft translucent button so it sits on the image without dominating.
		Ref<StyleBoxFlat> normal;
		normal.instantiate();
		normal->set_bg_color(Color(0, 0, 0, 0.45f));
		normal->set_corner_radius_all(int(6 * EDSCALE));
		normal->set_border_width_all(0);
		normal->set_content_margin_all(int(6 * EDSCALE));
		btn->add_theme_style_override("normal", normal);

		Ref<StyleBoxFlat> hover = normal->duplicate();
		hover->set_bg_color(Color(0, 0, 0, 0.65f));
		btn->add_theme_style_override("hover", hover);

		Ref<StyleBoxFlat> pressed = normal->duplicate();
		pressed->set_bg_color(Color(0, 0, 0, 0.80f));
		btn->add_theme_style_override("pressed", pressed);

		btn->add_theme_color_override("font_color", Color(1, 1, 1, 0.92f));
		btn->add_theme_color_override("font_hover_color", Color(1, 1, 1, 1.0f));

		btn->connect("pressed", callable_mp(this, &AIImageViewer::_on_arrow_pressed).bind(p_direction));
		_root->add_child(btn);
		return btn;
	};
	_btn_prev = build_arrow(String::utf8("❮"), -1); // ❮
	_btn_next = build_arrow(String::utf8("❯"), 1);  // ❯

	// Arrow buttons live at vertical center, hugging the popup edges. RIGHT
	// is computed as LEFT + button_width since CENTER_LEFT anchors RIGHT to
	// the parent's left edge as well.
	const float btn_w = 36 * EDSCALE;
	const float btn_h = 60 * EDSCALE;
	_btn_prev->set_anchors_preset(Control::PRESET_CENTER_LEFT);
	_btn_prev->set_offset(SIDE_LEFT, 12 * EDSCALE);
	_btn_prev->set_offset(SIDE_RIGHT, 12 * EDSCALE + btn_w);
	_btn_prev->set_offset(SIDE_TOP, -btn_h * 0.5f);
	_btn_prev->set_offset(SIDE_BOTTOM, btn_h * 0.5f);

	_btn_next->set_anchors_preset(Control::PRESET_CENTER_RIGHT);
	_btn_next->set_offset(SIDE_LEFT, -(12 * EDSCALE + btn_w));
	_btn_next->set_offset(SIDE_RIGHT, -12 * EDSCALE);
	_btn_next->set_offset(SIDE_TOP, -btn_h * 0.5f);
	_btn_next->set_offset(SIDE_BOTTOM, btn_h * 0.5f);

	// Catches arrow keys at the popup level — sidesteps the focus-stealing
	// trap that bites gui_input on popups.
	connect("window_input", callable_mp(this, &AIImageViewer::_on_window_input));

}

void AIImageViewer::_notification(int p_what) {
	// Reserved for future theme-refresh handling.
}

Ref<ImageTexture> AIImageViewer::_ensure_decoded(int p_index) {
	if (p_index < 0 || p_index >= _b64s.size()) {
		return Ref<ImageTexture>();
	}
	if (_tex_cache[p_index].is_valid()) {
		return _tex_cache[p_index];
	}
	const String &b64 = _b64s[p_index];
	if (b64.is_empty()) {
		return Ref<ImageTexture>();
	}
	PackedByteArray bytes = CoreBind::Marshalls::get_singleton()->base64_to_raw(b64);
	if (bytes.is_empty()) {
		return Ref<ImageTexture>();
	}
	Ref<Image> img;
	img.instantiate();
	if (img->load_png_from_buffer(bytes) != OK || img->is_empty()) {
		return Ref<ImageTexture>();
	}
	Ref<ImageTexture> tex = ImageTexture::create_from_image(img);
	_tex_cache.write[p_index] = tex;
	return tex;
}

void AIImageViewer::_update_arrow_visibility() {
	const int n = _b64s.size();
	const bool show_arrows = n >= 2;
	_btn_prev->set_visible(show_arrows && _index > 0);
	_btn_next->set_visible(show_arrows && _index < n - 1);
}

void AIImageViewer::_apply_index_immediately() {
	if (_active_tween.is_valid() && _active_tween->is_valid()) {
		Ref<Tween> prev = _active_tween;
		prev->kill();
		_active_tween.unref();
	}
	Ref<ImageTexture> tex = _ensure_decoded(_index);
	_active_layer = 0;
	_main_a->set_texture(tex);
	_main_a->set_offset(SIDE_LEFT, 0);
	_main_a->set_offset(SIDE_RIGHT, 0);
	_main_a->show();
	_main_b->set_texture(Ref<Texture2D>());
	_main_b->hide();
	_main_b->set_offset(SIDE_LEFT, 0);
	_main_b->set_offset(SIDE_RIGHT, 0);
	_update_arrow_visibility();
}

void AIImageViewer::_animate_to(int p_new_index, int p_direction) {
	const int n = _b64s.size();
	if (p_new_index < 0 || p_new_index >= n || n <= 1) {
		return;
	}
	if (_root == nullptr || _main_a == nullptr || _main_b == nullptr) {
		return;
	}

	// If a previous slide is still in flight, snap it to its end state before
	// starting the new one. We hold a local Ref because `custom_step` fires
	// chained callbacks synchronously, and `_on_tween_finished` clears
	// `_active_tween` from underneath us — operating on the local Ref keeps
	// the subsequent kill() call safe.
	if (_active_tween.is_valid() && _active_tween->is_valid()) {
		Ref<Tween> prev = _active_tween;
		prev->custom_step(10.0); // Force-complete; fires _on_tween_finished and clears _active_tween.
		if (prev->is_valid()) {
			prev->kill();
		}
	}

	// Until the popup has actually been sized (first popup_for_images call), a
	// slide animation can't compute its travel distance. Snap the index without
	// animating in that edge case rather than dispatching a 0-distance tween.
	const float popup_w = _root->get_size().x;
	if (popup_w <= 0.0f) {
		_index = p_new_index;
		_apply_index_immediately();
		return;
	}
	const float slide = popup_w; // Each layer travels one popup-width.

	TextureRect *outgoing = (_active_layer == 0) ? _main_a : _main_b;
	TextureRect *incoming = (_active_layer == 0) ? _main_b : _main_a;

	Ref<ImageTexture> new_tex = _ensure_decoded(p_new_index);
	incoming->set_texture(new_tex);

	// Direction +1 (next): outgoing slides left; incoming starts at +slide and
	// slides to 0. Direction -1 (prev): mirror.
	const float incoming_start = (p_direction > 0) ? slide : -slide;
	const float outgoing_end = (p_direction > 0) ? -slide : slide;

	incoming->set_offset(SIDE_LEFT, incoming_start);
	incoming->set_offset(SIDE_RIGHT, incoming_start);
	incoming->show();
	outgoing->set_offset(SIDE_LEFT, 0);
	outgoing->set_offset(SIDE_RIGHT, 0);

	const int new_active = (_active_layer == 0) ? 1 : 0;

	const float duration = 0.18f;
	Ref<Tween> tw = create_tween();
	tw->set_parallel(true);
	tw->set_trans(Tween::TRANS_CUBIC);
	tw->set_ease(Tween::EASE_OUT);
	tw->tween_property(incoming, NodePath("offset_left"), 0.0f, duration);
	tw->tween_property(incoming, NodePath("offset_right"), 0.0f, duration);
	tw->tween_property(outgoing, NodePath("offset_left"), outgoing_end, duration);
	tw->tween_property(outgoing, NodePath("offset_right"), outgoing_end, duration);
	tw->chain()->tween_callback(callable_mp(this, &AIImageViewer::_on_tween_finished).bind(new_active));
	_active_tween = tw;

	_index = p_new_index;
	_update_arrow_visibility();
}

void AIImageViewer::_on_tween_finished(int p_settled_active) {
	_active_layer = p_settled_active;
	TextureRect *outgoing = (p_settled_active == 0) ? _main_b : _main_a;
	outgoing->hide();
	outgoing->set_texture(Ref<Texture2D>());
	outgoing->set_offset(SIDE_LEFT, 0);
	outgoing->set_offset(SIDE_RIGHT, 0);
	if (_active_tween.is_valid()) {
		_active_tween.unref();
	}
}

void AIImageViewer::_go(int p_delta) {
	const int n = _b64s.size();
	if (n <= 1) {
		return;
	}
	int target = _index + p_delta;
	if (target < 0 || target >= n) {
		return;
	}
	_animate_to(target, p_delta > 0 ? 1 : -1);
}

void AIImageViewer::_on_arrow_pressed(int p_direction) {
	_go(p_direction);
}

void AIImageViewer::_on_window_input(const Ref<InputEvent> &p_event) {
	if (_b64s.size() <= 1) {
		return;
	}
	Ref<InputEventKey> key = p_event;
	if (key.is_valid() && key->is_pressed() && !key->is_echo()) {
		if (key->get_keycode() == Key::LEFT) {
			_go(-1);
		} else if (key->get_keycode() == Key::RIGHT) {
			_go(1);
		}
	}
}

void AIImageViewer::set_image_index(int p_index) {
	if (_b64s.is_empty()) {
		return;
	}
	const int clamped = CLAMP(p_index, 0, _b64s.size() - 1);
	if (clamped == _index) {
		return;
	}
	const int direction = (clamped > _index) ? 1 : -1;
	_animate_to(clamped, direction);
}

void AIImageViewer::popup_for_images(const Vector<String> &p_b64s, int p_start_index) {
	_b64s = p_b64s;
	_tex_cache.resize(_b64s.size());
	for (int i = 0; i < _tex_cache.size(); i++) {
		_tex_cache.write[i] = Ref<ImageTexture>();
	}
	_index = (p_b64s.is_empty()) ? 0 : CLAMP(p_start_index, 0, p_b64s.size() - 1);

	if (_b64s.is_empty()) {
		return;
	}

	// Decode the starting image up front so we can size the popup to its aspect.
	Ref<ImageTexture> start_tex = _ensure_decoded(_index);
	if (start_tex.is_null()) {
		return;
	}
	Size2 img_px = start_tex->get_size();
	if (img_px.y <= 0.0f) {
		return;
	}

	Viewport *parent_vp = get_embedder() ? get_embedder() : nullptr;
	Size2 vp_size;
	if (parent_vp) {
		vp_size = parent_vp->get_visible_rect().size;
	} else {
		Window *parent_win = Object::cast_to<Window>(get_parent());
		if (parent_win) {
			vp_size = parent_win->get_visible_rect().size;
		}
	}
	if (vp_size == Size2()) {
		vp_size = Size2(1280, 720);
	}

	Size2 cap = vp_size * 0.85f;
	float aspect = img_px.x / img_px.y;
	Size2 popup_size = cap;
	if (popup_size.x / aspect > cap.y) {
		popup_size.x = cap.y * aspect;
	} else {
		popup_size.y = popup_size.x / aspect;
	}

	set_size(popup_size);
	popup_centered();
	_apply_index_immediately();
}

// ============================================================================
// AIImageStack
// ============================================================================

void AIImageStack::_bind_methods() {
	// `set_images` and `get_image_count` are only invoked from C++ — no need
	// to expose them to GDScript / Variant marshalling. Variant has no type
	// info for `Vector<Ref<Texture2D>>` so binding set_images would fail.
	ADD_SIGNAL(MethodInfo("clicked"));
}

AIImageStack::AIImageStack() {
	set_mouse_filter(Control::MOUSE_FILTER_STOP);
	set_default_cursor_shape(Control::CURSOR_POINTING_HAND);
	connect("gui_input", callable_mp(this, &AIImageStack::_on_self_input));
	// Default thumbnail height — matches the previous body_screenshot TextureRect.
	// Set via custom_minimum_size (not a get_minimum_size override) so the chat
	// panel can animate the stack's height during the collapse/expand transition.
	set_custom_minimum_size(Size2(0, 200 * EDSCALE));
}

void AIImageStack::_ensure_shadow_style() {
	if (_shadow_style.is_valid()) {
		return;
	}
	_shadow_style.instantiate();
	// Transparent body — only the shadow + corner radius render. The TextureRect
	// children paint over the same rect, so the shadow only shows where it
	// extends past the rect (i.e. underneath the image).
	_shadow_style->set_bg_color(Color(0, 0, 0, 0));
	_shadow_style->set_corner_radius_all(int(6 * EDSCALE));
	_shadow_style->set_shadow_color(Color(0, 0, 0, 0.45f));
	_shadow_style->set_shadow_size(int(8 * EDSCALE));
	_shadow_style->set_shadow_offset(Vector2(0, 2) * EDSCALE);
}

void AIImageStack::_ensure_children() {
	// Lazily create up to 3 real-image layers and the placeholder. Children
	// outlive any individual set_images call — we just toggle visibility.
	while (_layers.size() < 3) {
		TextureRect *tr = memnew(TextureRect);
		tr->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
		tr->set_expand_mode(TextureRect::EXPAND_FIT_WIDTH_PROPORTIONAL);
		// Peek layers (anything but the top) ignore mouse events so the click
		// always lands on the AIImageStack itself.
		tr->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		tr->hide();
		add_child(tr);
		_layers.push_back(tr);
	}

	if (!_placeholder) {
		_placeholder = memnew(Panel);
		_placeholder->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		Ref<StyleBoxFlat> sb;
		sb.instantiate();
		sb->set_bg_color(Color(0.18f, 0.18f, 0.20f, 1.0f));
		sb->set_border_color(Color(0.30f, 0.30f, 0.34f, 1.0f));
		sb->set_border_width_all(1);
		sb->set_corner_radius_all(int(6 * EDSCALE));
		_placeholder->add_theme_style_override("panel", sb);
		_placeholder->hide();
		add_child(_placeholder);

		_more_label = memnew(Label);
		_more_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
		_more_label->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
		_more_label->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
		_more_label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		_more_label->add_theme_color_override("font_color", Color(0.85f, 0.85f, 0.88f, 1.0f));
		_placeholder->add_child(_more_label);
	}

	_ensure_shadow_style();
}

void AIImageStack::set_images(const Vector<Ref<Texture2D>> &p_textures) {
	_textures = p_textures;
	_count = p_textures.size();
	_ensure_children();

	const int visible_image_layers = MIN(_count, 3);
	const bool show_placeholder = _count >= 4;

	// _layers[0] is rendered as the top image (last in the source vector).
	// _layers[1] is the second-from-top peek (second-to-last source image).
	// _layers[2] is the deepest real-image peek (third-from-last source image).
	for (int i = 0; i < _layers.size(); i++) {
		const int layer_visible = i < visible_image_layers;
		if (layer_visible) {
			const int src_index = _count - 1 - i;
			_layers[i]->set_texture(_textures[src_index]);
			_layers[i]->show();
		} else {
			_layers[i]->set_texture(Ref<Texture2D>());
			_layers[i]->hide();
		}
	}

	if (show_placeholder) {
		_placeholder->show();
		_more_label->set_text(vformat("+%d", _count - 3));
	} else {
		_placeholder->hide();
	}

	if (_count == 0) {
		hide();
	} else {
		show();
	}
	_do_layout();
	queue_redraw();
}

void AIImageStack::_do_layout() {
	if (_count == 0) {
		return;
	}

	const Size2 sz = get_size();
	if (sz.x <= 0 || sz.y <= 0) {
		return;
	}

	const bool show_placeholder = _count >= 4;
	const int real_layers = MIN(_count, 3);
	const int total_layers = real_layers + (show_placeholder ? 1 : 0);

	const float peek_offset = 14 * EDSCALE;
	const float diag_offset = 3 * EDSCALE;

	const float reserved = peek_offset * (total_layers - 1);
	const Size2 top_size(MAX(0.0f, sz.x - reserved), MAX(0.0f, sz.y - diag_offset * (total_layers - 1)));

	for (int i = 0; i < real_layers; i++) {
		const Vector2 pos(i * peek_offset, i * diag_offset);
		_layers[i]->set_position(pos);
		_layers[i]->set_size(top_size);
	}
	for (int i = real_layers; i < _layers.size(); i++) {
		_layers[i]->hide();
	}

	if (show_placeholder) {
		const int idx = real_layers;
		const Vector2 pos(idx * peek_offset, idx * diag_offset);
		_placeholder->set_position(pos);
		_placeholder->set_size(top_size);
	}

	// Z-order: deepest layers first, top image last so it draws on top.
	if (show_placeholder) {
		move_child(_placeholder, 0);
	}
	for (int i = real_layers - 1; i >= 0; i--) {
		move_child(_layers[i], get_child_count() - 1);
	}
}

void AIImageStack::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_RESIZED:
			_do_layout();
			queue_redraw(); // Shadow extents follow layer rects.
			break;
		case NOTIFICATION_THEME_CHANGED:
			update_minimum_size();
			break;
		case NOTIFICATION_DRAW: {
			if (_count == 0 || _shadow_style.is_null() || _layers.size() < 3) {
				break;
			}
			const bool show_placeholder = _count >= 4;
			const int real_layers = MIN(_count, 3);

			// Paint the deepest layer first so the top layer's shadow visually
			// settles on top — order matches the layer drawing order.
			if (show_placeholder && _placeholder && _placeholder->is_visible()) {
				draw_style_box(_shadow_style, _placeholder->get_rect());
			}
			for (int i = real_layers - 1; i >= 0; i--) {
				if (!_layers[i] || !_layers[i]->is_visible()) {
					continue;
				}
				draw_style_box(_shadow_style, _layers[i]->get_rect());
			}
		} break;
	}
}

void AIImageStack::_on_self_input(const Ref<InputEvent> &p_event) {
	if (_count == 0) {
		return;
	}
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MouseButton::LEFT) {
		emit_signal("clicked");
		accept_event();
	}
}
