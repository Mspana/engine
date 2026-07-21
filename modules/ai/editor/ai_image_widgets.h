/**************************************************************************/
/*  ai_image_widgets.h                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#ifndef AI_IMAGE_WIDGETS_H
#define AI_IMAGE_WIDGETS_H

#include "core/templates/vector.h"
#include "scene/animation/tween.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/margin_container.h"
#include "scene/gui/panel.h"
#include "scene/gui/popup.h"
#include "scene/gui/texture_rect.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/style_box_flat.h"

// AIImageViewer — popup that displays one of N base64-encoded PNG images at a
// time. With N >= 2, on-screen prev/next arrow buttons (and Left/Right keys)
// navigate; transitions cross-slide so the outgoing image leaves while the
// incoming one slides in from the opposite side. With N == 1, arrow buttons
// hide and the popup looks identical to the previous single-image popup
// (clean dark panel, no border, image fills the popup at aspect-preserved
// centered).
class AIImageViewer : public PopupPanel {
	GDCLASS(AIImageViewer, PopupPanel);

	// Source data — base64 PNGs. Decoded lazily into _tex_cache as the user
	// navigates so the popup stays cheap to construct for large galleries.
	Vector<String> _b64s;
	Vector<Ref<ImageTexture>> _tex_cache;
	int _index = 0;
	int _active_layer = 0; // 0 -> _main_a is the displayed image at rest, 1 -> _main_b
	Ref<Tween> _active_tween;

	Control *_root = nullptr;
	// Two stacked TextureRects so we can cross-slide on navigate. At rest only
	// one is visible at offset (0,0); the other sits offscreen waiting for
	// its next role as the incoming layer.
	TextureRect *_main_a = nullptr;
	TextureRect *_main_b = nullptr;
	Button *_btn_prev = nullptr;
	Button *_btn_next = nullptr;

	Ref<ImageTexture> _ensure_decoded(int p_index);
	void _apply_index_immediately(); // No animation — used on popup_for_images.
	void _animate_to(int p_new_index, int p_direction); // p_direction: -1 prev, +1 next.
	void _update_arrow_visibility();
	void _on_window_input(const Ref<InputEvent> &p_event);
	void _on_arrow_pressed(int p_direction);
	void _go(int p_delta);
	void _on_tween_finished(int p_settled_active);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// Show the popup for `p_b64s` images, starting at `p_start_index`. The
	// popup sizes itself to ~85% of the viewport while preserving the current
	// image's aspect ratio.
	void popup_for_images(const Vector<String> &p_b64s, int p_start_index = 0);

	// Set the visible image index. Out-of-range values are clamped silently.
	// Named `set_image_index` rather than `set_index` because Node already has
	// a `get_index()` method (sibling position) — registering a clashing
	// `get_index` on a Node subclass throws "already has a method" and leaves
	// the class half-wired, which downstream ClassDB lookups crash on.
	void set_image_index(int p_index);
	int get_image_index() const { return _index; }

	int get_image_count() const { return _b64s.size(); }

	AIImageViewer();
};

// AIImageStack — inline thumbnail control that displays 1..N images in an
// iMessage-style stack. With one image it renders identically to the previous
// single TextureRect: full-rect, aspect-preserved, expand-fit-width. With more
// images, the top image is image[N-1] and earlier images peek out behind to
// the right with a small diagonal offset; for N >= 4 a placeholder layer
// peeks farthest with a "+M" label indicating extras beyond the visible 3.
//
// A soft drop shadow with rounded corners is drawn behind every visible layer
// so the stack reads as a row of cards.
//
// Hit-testing is single-target: peek layers ignore mouse events, the Control
// itself listens for left-clicks on its own rect and emits `clicked` so the
// owner can spawn a viewer.
class AIImageStack : public Control {
	GDCLASS(AIImageStack, Control);

	// Caller decodes textures up front (so the chat-bubble layer can keep its
	// existing decode pipeline). Holds at most 4 visible layers regardless of
	// the input vector's length — extras beyond 3 are summarised by a single
	// placeholder + count label.
	Vector<Ref<Texture2D>> _textures;
	int _count = 0;

	// Layered children. _layers stores up to 3 real-image rects (idx 0 = top
	// rendered last in z-order; idx 2 = farthest peek). _placeholder is the
	// rounded "+M" panel for N >= 4. All are children of `this`.
	Vector<TextureRect *> _layers;
	Panel *_placeholder = nullptr;
	Label *_more_label = nullptr;

	// Stylebox painted behind each visible layer in _draw() — gives the stack
	// its drop-shadow + rounded-corner card look without clipping the actual
	// image texture (which would require a shader).
	Ref<StyleBoxFlat> _shadow_style;

	void _ensure_children();
	void _ensure_shadow_style();
	void _do_layout();
	void _on_self_input(const Ref<InputEvent> &p_event);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_images(const Vector<Ref<Texture2D>> &p_textures);
	int get_image_count() const { return _count; }

	AIImageStack();
};

#endif // AI_IMAGE_WIDGETS_H
