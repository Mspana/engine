/**************************************************************************/
/*  ai_remote_dialog.h                                                    */
/**************************************************************************/
/* Editor UI for remote access: the on/off switch, the pairing window,    */
/* and the paired-device list.                                            */
/*                                                                        */
/* Granting a device control lives here, on the desktop, deliberately —   */
/* a paired phone is view-only until someone at the machine says          */
/* otherwise.                                                             */
/**************************************************************************/

#ifndef AI_REMOTE_DIALOG_H
#define AI_REMOTE_DIALOG_H

#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/option_button.h"
#include "scene/gui/rich_text_label.h"
#include "scene/gui/texture_rect.h"
#include "scene/main/timer.h"

class AIRemoteDialog : public AcceptDialog {
	GDCLASS(AIRemoteDialog, AcceptDialog);

	Label *status_label = nullptr;
	LineEdit *url_field = nullptr;
	OptionButton *mode_dropdown = nullptr;
	Button *toggle_button = nullptr;

	// Which local address to advertise. Auto-ranked, but overridable because a
	// machine with a VPN or hypervisor has several and the guess can be wrong.
	HBoxContainer *address_row = nullptr;
	OptionButton *address_dropdown = nullptr;
	void _rebuild_address_dropdown();
	void _on_address_selected(int p_index);

	TextureRect *qr_image = nullptr;
	Label *qr_hint = nullptr;
	void _update_qr(const String &p_payload);

	VBoxContainer *pairing_section = nullptr;
	Button *pair_button = nullptr;
	LineEdit *pair_code_field = nullptr;
	Label *pair_countdown = nullptr;
	Button *pair_cancel_button = nullptr;

	VBoxContainer *device_list = nullptr;
	Label *device_empty_label = nullptr;
	Button *revoke_all_button = nullptr;

	Timer *refresh_timer = nullptr;

	void _on_toggle_pressed();
	void _on_pair_pressed();
	void _on_pair_cancel_pressed();
	void _on_revoke_all_pressed();
	void _on_device_control_toggled(bool p_pressed, const String &p_id);
	void _on_device_revoke_pressed(const String &p_id);
	void _on_refresh_tick();

	void _rebuild_devices();
	void _update_state();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void open();
	AIRemoteDialog();
};

#endif // AI_REMOTE_DIALOG_H
