/**************************************************************************/
/*  ai_remote_devices.h                                                   */
/**************************************************************************/
/* Paired-device registry and server identity for remote access.          */
/*                                                                        */
/* Trust model: a device is identified by its X25519 public key. Pairing   */
/* adds the key here; revoking removes it and kills that device's live     */
/* sessions. Devices are VIEW-ONLY until allow_control is granted from the */
/* desktop editor — pairing a phone never by itself lets it drive the      */
/* agent.                                                                  */
/**************************************************************************/

#ifndef AI_REMOTE_DEVICES_H
#define AI_REMOTE_DEVICES_H

#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/variant/variant.h"

struct AIRemoteDevice {
	String id; // fingerprint of the public key
	String name;
	PackedByteArray public_key;
	int64_t paired_at = 0;
	int64_t last_seen = 0;
	int64_t expires_at = 0; // 0 = never
	bool allow_control = false;

	Dictionary to_dict() const;
	static AIRemoteDevice from_dict(const Dictionary &p_d);
	bool is_expired(int64_t p_now_ms) const { return expires_at > 0 && p_now_ms > expires_at; }
};

class AIRemoteDeviceStore {
	Vector<AIRemoteDevice> devices;
	PackedByteArray server_private;
	PackedByteArray server_public;
	bool loaded = false;

	static String _dir() { return "user://ai_remote"; }
	static String _devices_path() { return "user://ai_remote/devices.json"; }
	static String _identity_path() { return "user://ai_remote/server_key.json"; }
	bool _ensure_dir() const;

public:
	// Device keys live this long before a re-pair is required.
	static const int64_t DEFAULT_DEVICE_TTL_MS = 90LL * 24 * 60 * 60 * 1000;

	void load();
	bool save() const;

	// Server identity, generated on first use and persisted.
	bool ensure_identity();
	const PackedByteArray &get_server_private() const { return server_private; }
	const PackedByteArray &get_server_public() const { return server_public; }

	const Vector<AIRemoteDevice> &get_devices() const { return devices; }
	const AIRemoteDevice *find_by_public_key(const PackedByteArray &p_public) const;
	const AIRemoteDevice *find_by_id(const String &p_id) const;

	// Returns the new device's id, or empty on failure. Replaces any existing
	// entry with the same public key (re-pairing refreshes the expiry).
	String add_device(const PackedByteArray &p_public, const String &p_name);
	bool remove_device(const String &p_id);
	void remove_all();
	bool set_allow_control(const String &p_id, bool p_allow);
	void touch(const String &p_id, int64_t p_now_ms);

	// Append-only audit trail at user://ai_remote/audit.jsonl.
	static void audit(const String &p_event, const Dictionary &p_details);
};

#endif // AI_REMOTE_DEVICES_H
