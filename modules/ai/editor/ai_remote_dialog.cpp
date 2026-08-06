/**************************************************************************/
/*  ai_remote_dialog.cpp                                                  */
/**************************************************************************/

#include "ai_remote_dialog.h"

#include "../remote/ai_remote_qr.h"
#include "../remote/ai_remote_server.h"
#include "editor/themes/aristotle_tokens.h"

#include "core/io/image.h"
#include "core/os/time.h"
#include "scene/resources/image_texture.h"
#include "editor/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/separator.h"

// Display size for the pairing QR; the module bitmap is scaled to fit by a
// whole-number factor so it stays crisp.
static const int QR_DISPLAY_PX = 260;

void AIRemoteDialog::_bind_methods() {}

AIRemoteDialog::AIRemoteDialog() {
	set_title(TTR("Remote Access"));
	set_min_size(Size2(560, 520) * EDSCALE);

	VBoxContainer *root = memnew(VBoxContainer);
	root->add_theme_constant_override("separation", 10 * EDSCALE);
	add_child(root);

	// ---- Explanation --------------------------------------------------------
	RichTextLabel *intro = memnew(RichTextLabel);
	intro->set_use_bbcode(true);
	intro->set_fit_content(true);
	intro->set_selection_enabled(true);
	intro->set_text(TTR(
			"View and continue AI chats from your phone or another browser on this network.\n"
			"Your machine never opens a port to the internet — only devices on your local network can reach it, "
			"and every paired device must be approved here first."));
	root->add_child(intro);

	// ---- Status + controls --------------------------------------------------
	status_label = memnew(Label);
	status_label->set_text(TTR("Remote access is off."));
	root->add_child(status_label);

	HBoxContainer *controls = memnew(HBoxContainer);
	controls->add_theme_constant_override("separation", 8 * EDSCALE);
	root->add_child(controls);

	Label *mode_label = memnew(Label);
	mode_label->set_text(TTR("Reachable from:"));
	controls->add_child(mode_label);

	mode_dropdown = memnew(OptionButton);
	mode_dropdown->add_item(TTR("This computer only"), AIRemoteServer::BIND_LOOPBACK);
	mode_dropdown->add_item(TTR("This local network"), AIRemoteServer::BIND_LAN);
	mode_dropdown->select(0);
	controls->add_child(mode_dropdown);

	toggle_button = memnew(Button);
	toggle_button->set_text(TTR("Enable"));
	toggle_button->connect("pressed", callable_mp(this, &AIRemoteDialog::_on_toggle_pressed));
	controls->add_child(toggle_button);

	address_row = memnew(HBoxContainer);
	address_row->add_theme_constant_override("separation", 8 * EDSCALE);
	root->add_child(address_row);
	Label *address_label = memnew(Label);
	address_label->set_text(TTR("Network address:"));
	address_row->add_child(address_label);
	address_dropdown = memnew(OptionButton);
	address_dropdown->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	address_dropdown->set_tooltip_text(TTR(
			"Which of this machine's addresses to advertise. VPN and virtual adapters are listed "
			"last because a phone usually cannot reach them."));
	address_dropdown->connect("item_selected", callable_mp(this, &AIRemoteDialog::_on_address_selected));
	address_row->add_child(address_dropdown);

	HBoxContainer *url_row = memnew(HBoxContainer);
	root->add_child(url_row);
	Label *url_label = memnew(Label);
	url_label->set_text(TTR("Open on your device:"));
	url_row->add_child(url_label);
	url_field = memnew(LineEdit);
	url_field->set_editable(false);
	url_field->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	url_field->set_placeholder(TTR("Enable remote access to see the address"));
	url_row->add_child(url_field);

	root->add_child(memnew(HSeparator));

	// ---- Pairing ------------------------------------------------------------
	pairing_section = memnew(VBoxContainer);
	pairing_section->add_theme_constant_override("separation", 6 * EDSCALE);
	root->add_child(pairing_section);

	Label *pair_title = memnew(Label);
	pair_title->set_text(TTR("Pair a device"));
	pair_title->add_theme_color_override("font_color", Aristotle::TEXT_PRIMARY);
	pairing_section->add_child(pair_title);

	Label *pair_hint = memnew(Label);
	pair_hint->set_text(TTR(
			"Scan the code with your phone's camera — it carries the address and the pairing code together. "
			"Or open the address above and type the code by hand. Either way it works once and expires in two minutes."));
	pair_hint->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	pair_hint->add_theme_color_override("font_color", Aristotle::TEXT_SECONDARY);
	pairing_section->add_child(pair_hint);

	HBoxContainer *pair_row = memnew(HBoxContainer);
	pair_row->add_theme_constant_override("separation", 8 * EDSCALE);
	pairing_section->add_child(pair_row);

	pair_button = memnew(Button);
	pair_button->set_text(TTR("Generate pairing code"));
	pair_button->connect("pressed", callable_mp(this, &AIRemoteDialog::_on_pair_pressed));
	pair_row->add_child(pair_button);

	pair_code_field = memnew(LineEdit);
	pair_code_field->set_editable(false);
	pair_code_field->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	pair_code_field->set_visible(false);
	pair_row->add_child(pair_code_field);

	pair_countdown = memnew(Label);
	pair_countdown->set_visible(false);
	pair_row->add_child(pair_countdown);

	pair_cancel_button = memnew(Button);
	pair_cancel_button->set_text(TTR("Cancel"));
	pair_cancel_button->set_visible(false);
	pair_cancel_button->connect("pressed", callable_mp(this, &AIRemoteDialog::_on_pair_cancel_pressed));
	pair_row->add_child(pair_cancel_button);

	// Scanning the QR carries both the address and the code, so the phone never
	// has to type either.
	qr_image = memnew(TextureRect);
	qr_image->set_visible(false);
	qr_image->set_custom_minimum_size(Size2(QR_DISPLAY_PX, QR_DISPLAY_PX) * EDSCALE);
	qr_image->set_stretch_mode(TextureRect::STRETCH_KEEP);
	qr_image->set_texture_filter(CanvasItem::TEXTURE_FILTER_NEAREST);
	pairing_section->add_child(qr_image);

	qr_hint = memnew(Label);
	qr_hint->set_text(TTR("Scan with your phone's camera, or type the code above."));
	qr_hint->set_visible(false);
	qr_hint->add_theme_color_override("font_color", Aristotle::TEXT_SECONDARY);
	pairing_section->add_child(qr_hint);

	root->add_child(memnew(HSeparator));

	// ---- Devices ------------------------------------------------------------
	HBoxContainer *devices_header = memnew(HBoxContainer);
	root->add_child(devices_header);
	Label *devices_title = memnew(Label);
	devices_title->set_text(TTR("Paired devices"));
	devices_title->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	devices_header->add_child(devices_title);
	revoke_all_button = memnew(Button);
	revoke_all_button->set_text(TTR("Revoke all"));
	revoke_all_button->connect("pressed", callable_mp(this, &AIRemoteDialog::_on_revoke_all_pressed));
	devices_header->add_child(revoke_all_button);

	Label *control_hint = memnew(Label);
	control_hint->set_text(TTR(
			"Paired devices can watch chats. \"Allow control\" also lets them send messages, cancel runs and answer "
			"approval prompts — grant it only to devices you physically hold."));
	control_hint->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	control_hint->add_theme_color_override("font_color", Aristotle::TEXT_SECONDARY);
	root->add_child(control_hint);

	device_list = memnew(VBoxContainer);
	device_list->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	root->add_child(device_list);

	device_empty_label = memnew(Label);
	device_empty_label->set_text(TTR("No devices paired yet."));
	device_empty_label->add_theme_color_override("font_color", Aristotle::TEXT_SECONDARY);
	device_list->add_child(device_empty_label);

	refresh_timer = memnew(Timer);
	refresh_timer->set_wait_time(1.0);
	refresh_timer->set_autostart(false);
	refresh_timer->connect("timeout", callable_mp(this, &AIRemoteDialog::_on_refresh_tick));
	add_child(refresh_timer);
}

void AIRemoteDialog::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_VISIBILITY_CHANGED: {
			if (is_visible()) {
				refresh_timer->start();
			} else {
				refresh_timer->stop();
			}
		} break;
	}
}

void AIRemoteDialog::open() {
	// Restore a previously chosen address before anything reads the URL.
	if (AIRemoteServer *server = AIRemoteServer::get_singleton()) {
		const String saved = EditorSettings::get_singleton()->get_project_metadata(
				"ai_remote", "preferred_address", "");
		if (!saved.is_empty()) {
			server->set_preferred_address(saved);
		}
	}
	_rebuild_address_dropdown();
	_update_state();
	_rebuild_devices();
	popup_centered();
}

void AIRemoteDialog::_on_refresh_tick() {
	_update_state();
}

void AIRemoteDialog::_rebuild_address_dropdown() {
	AIRemoteServer *server = AIRemoteServer::get_singleton();
	if (!server || !address_dropdown) {
		return;
	}
	const String current = server->resolve_lan_address();
	const Array candidates = AIRemoteServer::list_lan_addresses();

	address_dropdown->clear();
	if (candidates.is_empty()) {
		address_dropdown->add_item(TTR("No local network address found"));
		address_dropdown->set_disabled(true);
		return;
	}
	address_dropdown->set_disabled(false);
	for (int i = 0; i < candidates.size(); i++) {
		Dictionary c = candidates[i];
		const String address = c.get("address", "");
		const String iface = c.get("interface", "");
		address_dropdown->add_item(iface.is_empty() ? address : vformat("%s — %s", address, iface), i);
		address_dropdown->set_item_metadata(i, address);
		if (address == current) {
			address_dropdown->select(i);
		}
	}
}

void AIRemoteDialog::_on_address_selected(int p_index) {
	AIRemoteServer *server = AIRemoteServer::get_singleton();
	if (!server || !address_dropdown) {
		return;
	}
	const String address = address_dropdown->get_item_metadata(p_index);
	if (address.is_empty()) {
		return;
	}
	server->set_preferred_address(address);
	EditorSettings::get_singleton()->set_project_metadata("ai_remote", "preferred_address", address);
	_update_state();
}

void AIRemoteDialog::_update_qr(const String &p_payload) {
	if (!qr_image) {
		return;
	}
	int modules = 0;
	PackedByteArray cells;
	if (p_payload.is_empty() || !AIRemoteQR::encode(p_payload, modules, cells)) {
		qr_image->set_visible(false);
		qr_hint->set_visible(false);
		return;
	}

	// A quiet zone is part of the spec, not decoration — scanners need it.
	const int QUIET = 4;
	const int side = modules + QUIET * 2;
	Ref<Image> img = Image::create_empty(side, side, false, Image::FORMAT_RGB8);
	img->fill(Color(1, 1, 1));
	for (int y = 0; y < modules; y++) {
		for (int x = 0; x < modules; x++) {
			if (cells[y * modules + x]) {
				img->set_pixel(x + QUIET, y + QUIET, Color(0, 0, 0));
			}
		}
	}

	// Scale by a whole number so every module stays a crisp square block.
	const int scale = MAX(1, (int)((QR_DISPLAY_PX * EDSCALE) / side));
	img->resize(side * scale, side * scale, Image::INTERPOLATE_NEAREST);

	qr_image->set_texture(ImageTexture::create_from_image(img));
	qr_image->set_visible(true);
	qr_hint->set_visible(true);
}

void AIRemoteDialog::_update_state() {
	AIRemoteServer *server = AIRemoteServer::get_singleton();
	if (!server) {
		status_label->set_text(TTR("Remote access is unavailable in this build."));
		toggle_button->set_disabled(true);
		return;
	}

	const bool running = server->is_running();
	toggle_button->set_text(running ? TTR("Disable") : TTR("Enable"));
	mode_dropdown->set_disabled(running);

	// The address only matters when other devices have to reach this machine.
	const bool lan = mode_dropdown->get_selected_id() == AIRemoteServer::BIND_LAN;
	address_row->set_visible(lan);
	if (lan && address_dropdown->get_item_count() == 0) {
		_rebuild_address_dropdown();
	}

	if (running) {
		const int active = server->get_active_connection_count();
		status_label->set_text(active > 0
						? vformat(TTR("Remote access is ON — %d device(s) connected."), active)
						: TTR("Remote access is ON — waiting for a device."));
		status_label->add_theme_color_override("font_color", Aristotle::STATUS_SUCCESS);
		url_field->set_text(server->get_client_url());
	} else {
		status_label->set_text(TTR("Remote access is off."));
		status_label->add_theme_color_override("font_color", Aristotle::TEXT_SECONDARY);
		url_field->set_text("");
	}

	pair_button->set_disabled(!running);

	const bool pairing = server->is_pairing_active();
	pair_code_field->set_visible(pairing);
	pair_countdown->set_visible(pairing);
	pair_cancel_button->set_visible(pairing);
	pair_button->set_visible(!pairing);
	if (pairing) {
		pair_countdown->set_text(vformat(TTR("expires in %ds"), server->get_pairing_seconds_left()));
	} else if (!pair_code_field->get_text().is_empty()) {
		// The window closed (paired, expired, or cancelled) — stop showing a
		// code, and a QR, that no longer work.
		pair_code_field->set_text("");
		_update_qr(String());
		_rebuild_devices();
	}
}

void AIRemoteDialog::_on_toggle_pressed() {
	AIRemoteServer *server = AIRemoteServer::get_singleton();
	if (!server) {
		return;
	}
	if (server->is_running()) {
		server->stop();
	} else {
		const int mode = mode_dropdown->get_selected_id();
		Error err = server->start(mode, AIRemoteServer::DEFAULT_PORT);
		if (err != OK) {
			status_label->set_text(TTR("Could not start — is the port already in use?"));
			status_label->add_theme_color_override("font_color", Aristotle::STATUS_ERROR);
			return;
		}
	}
	_update_state();
}

void AIRemoteDialog::_on_pair_pressed() {
	AIRemoteServer *server = AIRemoteServer::get_singleton();
	if (!server) {
		return;
	}
	const String code = server->begin_pairing();
	if (code.is_empty()) {
		return;
	}
	pair_code_field->set_text(code);
	// The code rides in the fragment: fragments are never sent to the server,
	// and the client strips it from history as soon as it has read it.
	_update_qr(vformat("%s/#c=%s", server->get_client_url(), code));
	_update_state();
}

void AIRemoteDialog::_on_pair_cancel_pressed() {
	if (AIRemoteServer *server = AIRemoteServer::get_singleton()) {
		server->cancel_pairing();
	}
	pair_code_field->set_text("");
	_update_state();
}

void AIRemoteDialog::_on_revoke_all_pressed() {
	if (AIRemoteServer *server = AIRemoteServer::get_singleton()) {
		server->revoke_all_devices();
	}
	_rebuild_devices();
}

void AIRemoteDialog::_on_device_control_toggled(bool p_pressed, const String &p_id) {
	if (AIRemoteServer *server = AIRemoteServer::get_singleton()) {
		server->set_device_control(p_id, p_pressed);
	}
}

void AIRemoteDialog::_on_device_revoke_pressed(const String &p_id) {
	if (AIRemoteServer *server = AIRemoteServer::get_singleton()) {
		server->revoke_device(p_id);
	}
	_rebuild_devices();
}

void AIRemoteDialog::_rebuild_devices() {
	for (int i = device_list->get_child_count() - 1; i >= 0; i--) {
		Node *child = device_list->get_child(i);
		device_list->remove_child(child);
		memdelete(child);
	}
	device_empty_label = nullptr;

	AIRemoteServer *server = AIRemoteServer::get_singleton();
	Array devices = server ? server->get_devices() : Array();

	if (devices.is_empty()) {
		device_empty_label = memnew(Label);
		device_empty_label->set_text(TTR("No devices paired yet."));
		device_empty_label->add_theme_color_override("font_color", Aristotle::TEXT_SECONDARY);
		device_list->add_child(device_empty_label);
		revoke_all_button->set_disabled(true);
		return;
	}
	revoke_all_button->set_disabled(false);

	for (int i = 0; i < devices.size(); i++) {
		Dictionary dev = devices[i];
		const String id = dev.get("id", "");

		PanelContainer *row_panel = memnew(PanelContainer);
		device_list->add_child(row_panel);

		HBoxContainer *row = memnew(HBoxContainer);
		row->add_theme_constant_override("separation", 8 * EDSCALE);
		row_panel->add_child(row);

		VBoxContainer *info = memnew(VBoxContainer);
		info->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		row->add_child(info);

		Label *name = memnew(Label);
		name->set_text(String(dev.get("name", "device")));
		info->add_child(name);

		Label *meta = memnew(Label);
		const int64_t last_seen = dev.get("last_seen", (int64_t)0);
		String seen_text = TTR("never connected");
		if (last_seen > 0) {
			const int64_t now = (int64_t)(Time::get_singleton()->get_unix_time_from_system() * 1000.0);
			const int64_t mins = (now - last_seen) / 60000;
			if (mins < 1) {
				seen_text = TTR("active now");
			} else if (mins < 60) {
				seen_text = vformat(TTR("%d min ago"), (int)mins);
			} else if (mins < 60 * 24) {
				seen_text = vformat(TTR("%d h ago"), (int)(mins / 60));
			} else {
				seen_text = vformat(TTR("%d days ago"), (int)(mins / (60 * 24)));
			}
		}
		meta->set_text(vformat("%s · %s", id.substr(0, 8), seen_text));
		meta->add_theme_color_override("font_color", Aristotle::TEXT_SECONDARY);
		info->add_child(meta);

		CheckBox *control = memnew(CheckBox);
		control->set_text(TTR("Allow control"));
		control->set_pressed(dev.get("allow_control", false));
		control->connect("toggled",
				callable_mp(this, &AIRemoteDialog::_on_device_control_toggled).bind(id));
		row->add_child(control);

		Button *revoke = memnew(Button);
		revoke->set_text(TTR("Revoke"));
		revoke->connect("pressed",
				callable_mp(this, &AIRemoteDialog::_on_device_revoke_pressed).bind(id));
		row->add_child(revoke);
	}
}
