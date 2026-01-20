/**************************************************************************/
/*  ai_status_indicator.h                                                 */
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

#ifndef AI_STATUS_INDICATOR_H
#define AI_STATUS_INDICATOR_H

#include "ai_chat_store.h"
#include "editor/plugins/editor_plugin.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/color_rect.h"
#include "scene/gui/label.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/separator.h"
#include "scene/gui/text_edit.h"
#include "scene/main/http_request.h"
#include "scene/main/timer.h"

class AIStatusIndicator : public ColorRect {
	GDCLASS(AIStatusIndicator, ColorRect);

public:
	enum Status {
		STATUS_UNKNOWN,
		STATUS_CHECKING,
		STATUS_CONNECTED,
		STATUS_DISCONNECTED
	};

private:
	Status current_status = STATUS_UNKNOWN;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_status(Status p_status);
	Status get_status() const;

	AIStatusIndicator();
};

class AIStatusPanel : public VBoxContainer {
	GDCLASS(AIStatusPanel, VBoxContainer);

private:
	// Chat store for persistence
	Ref<AIChatStore> chat_store;

	// Chat transcript UI
	ScrollContainer *transcript_scroll = nullptr;
	VBoxContainer *message_list = nullptr;
	Control *pending_message = nullptr;

	// Input area
	TextEdit *prompt_edit = nullptr;
	Button *send_button = nullptr;
	Button *clear_button = nullptr;

	// Status bar
	AIStatusIndicator *status_indicator = nullptr;
	Label *status_label = nullptr;

	// HTTP requests for checking each provider
	HTTPRequest *http_openai = nullptr;
	HTTPRequest *http_gemini = nullptr;
	HTTPRequest *http_xai = nullptr;

	// Track check results
	bool openai_connected = false;
	bool gemini_connected = false;
	bool xai_connected = false;
	int pending_checks = 0;

	// Chat state
	bool is_waiting_for_response = false;

	// UI building methods
	void _rebuild_message_list();
	void _append_message_ui(const ChatMessage &p_message);
	Control *_create_message_bubble(const ChatMessage &p_message);
	void _scroll_to_bottom();
	void _update_send_button_state();

	// Event handlers
	void _on_send_pressed();
	void _on_clear_pressed();
	void _on_prompt_text_changed();
	void _on_ai_response(bool p_success, const String &p_response, const String &p_error);

	// Pending message helpers
	void _show_pending_message();
	void _remove_pending_message();

	// Connectivity check handlers
	void _on_openai_request_completed(int p_result, int p_code, const PackedStringArray &p_headers, const PackedByteArray &p_body);
	void _on_gemini_request_completed(int p_result, int p_code, const PackedStringArray &p_headers, const PackedByteArray &p_body);
	void _on_xai_request_completed(int p_result, int p_code, const PackedStringArray &p_headers, const PackedByteArray &p_body);
	void _update_status_from_results();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void check_api_connectivity();

	AIStatusPanel();
	~AIStatusPanel();
};

class AIStatusIndicatorPlugin : public EditorPlugin {
	GDCLASS(AIStatusIndicatorPlugin, EditorPlugin);

private:
	AIStatusPanel *panel = nullptr;
	Timer *fallback_timer = nullptr;

	void _on_fallback_timer_timeout();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	virtual String get_plugin_name() const override { return "AI"; }
	virtual bool has_main_screen() const override { return false; }

	AIStatusIndicatorPlugin();
	~AIStatusIndicatorPlugin();
};

VARIANT_ENUM_CAST(AIStatusIndicator::Status);

#endif // AI_STATUS_INDICATOR_H
