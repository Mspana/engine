/**************************************************************************/
/*  ai_remote_devices.cpp                                                 */
/**************************************************************************/

#include "ai_remote_devices.h"

#include "ai_remote_crypto.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/time.h"

static int64_t _now_ms() {
	return (int64_t)(Time::get_singleton()->get_unix_time_from_system() * 1000.0);
}

// ---------------------------------------------------------------------------
// AIRemoteDevice
// ---------------------------------------------------------------------------

Dictionary AIRemoteDevice::to_dict() const {
	Dictionary d;
	d["id"] = id;
	d["name"] = name;
	d["public"] = AIRemoteCrypto::b64_encode(public_key);
	d["paired_at"] = paired_at;
	d["last_seen"] = last_seen;
	d["expires_at"] = expires_at;
	d["allow_control"] = allow_control;
	return d;
}

AIRemoteDevice AIRemoteDevice::from_dict(const Dictionary &p_d) {
	AIRemoteDevice dev;
	dev.id = p_d.get("id", "");
	dev.name = p_d.get("name", "");
	dev.public_key = AIRemoteCrypto::b64_decode(p_d.get("public", ""));
	dev.paired_at = p_d.get("paired_at", (int64_t)0);
	dev.last_seen = p_d.get("last_seen", (int64_t)0);
	dev.expires_at = p_d.get("expires_at", (int64_t)0);
	dev.allow_control = p_d.get("allow_control", false);
	return dev;
}

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

bool AIRemoteDeviceStore::_ensure_dir() const {
	if (DirAccess::exists(_dir())) {
		return true;
	}
	Ref<DirAccess> da = DirAccess::open("user://");
	ERR_FAIL_COND_V(da.is_null(), false);
	return da->make_dir_recursive("ai_remote") == OK;
}

void AIRemoteDeviceStore::load() {
	devices.clear();
	loaded = true;

	if (!FileAccess::exists(_devices_path())) {
		return;
	}
	Ref<FileAccess> f = FileAccess::open(_devices_path(), FileAccess::READ);
	if (f.is_null()) {
		return;
	}
	JSON json;
	if (json.parse(f->get_as_text()) != OK || json.get_data().get_type() != Variant::DICTIONARY) {
		WARN_PRINT("AIRemoteDeviceStore: devices.json is malformed; ignoring it.");
		return;
	}
	Dictionary root = json.get_data();
	Array list = root.get("devices", Array());
	for (int i = 0; i < list.size(); i++) {
		if (list[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		AIRemoteDevice dev = AIRemoteDevice::from_dict(list[i]);
		if (dev.public_key.size() == AIRemoteCrypto::X25519_KEY_SIZE && !dev.id.is_empty()) {
			devices.push_back(dev);
		}
	}
}

bool AIRemoteDeviceStore::save() const {
	if (!_ensure_dir()) {
		return false;
	}
	Array list;
	for (int i = 0; i < devices.size(); i++) {
		list.push_back(devices[i].to_dict());
	}
	Dictionary root;
	root["version"] = 1;
	root["devices"] = list;

	Ref<FileAccess> f = FileAccess::open(_devices_path(), FileAccess::WRITE);
	ERR_FAIL_COND_V(f.is_null(), false);
	f->store_string(JSON::stringify(root, "  "));
	return true;
}

bool AIRemoteDeviceStore::ensure_identity() {
	if (server_private.size() == AIRemoteCrypto::X25519_KEY_SIZE) {
		return true;
	}
	if (!_ensure_dir()) {
		return false;
	}

	if (FileAccess::exists(_identity_path())) {
		Ref<FileAccess> f = FileAccess::open(_identity_path(), FileAccess::READ);
		if (f.is_valid()) {
			JSON json;
			if (json.parse(f->get_as_text()) == OK && json.get_data().get_type() == Variant::DICTIONARY) {
				Dictionary d = json.get_data();
				server_private = AIRemoteCrypto::b64_decode(d.get("private", ""));
				server_public = AIRemoteCrypto::b64_decode(d.get("public", ""));
				if (server_private.size() == AIRemoteCrypto::X25519_KEY_SIZE &&
						server_public.size() == AIRemoteCrypto::X25519_KEY_SIZE) {
					return true;
				}
			}
			WARN_PRINT("AIRemoteDeviceStore: server_key.json unusable; regenerating identity.");
		}
	}

	if (!AIRemoteCrypto::generate_keypair(server_private, server_public)) {
		ERR_PRINT("AIRemoteDeviceStore: failed to generate server identity.");
		return false;
	}

	Dictionary d;
	d["version"] = 1;
	d["private"] = AIRemoteCrypto::b64_encode(server_private);
	d["public"] = AIRemoteCrypto::b64_encode(server_public);

	Ref<FileAccess> f = FileAccess::open(_identity_path(), FileAccess::WRITE);
	if (f.is_null()) {
		ERR_PRINT("AIRemoteDeviceStore: cannot write server identity.");
		return false;
	}
	f->store_string(JSON::stringify(d, "  "));
	// Regenerating the identity invalidates every previous pairing.
	audit("identity_generated", Dictionary());
	return true;
}

const AIRemoteDevice *AIRemoteDeviceStore::find_by_public_key(const PackedByteArray &p_public) const {
	if (p_public.size() != AIRemoteCrypto::X25519_KEY_SIZE) {
		return nullptr;
	}
	for (int i = 0; i < devices.size(); i++) {
		if (AIRemoteCrypto::const_time_equals(devices[i].public_key, p_public)) {
			return &devices[i];
		}
	}
	return nullptr;
}

const AIRemoteDevice *AIRemoteDeviceStore::find_by_id(const String &p_id) const {
	for (int i = 0; i < devices.size(); i++) {
		if (devices[i].id == p_id) {
			return &devices[i];
		}
	}
	return nullptr;
}

String AIRemoteDeviceStore::add_device(const PackedByteArray &p_public, const String &p_name) {
	ERR_FAIL_COND_V(p_public.size() != AIRemoteCrypto::X25519_KEY_SIZE, String());

	String id = AIRemoteCrypto::fingerprint(p_public);
	if (id.is_empty()) {
		return String();
	}
	const int64_t now = _now_ms();

	// Re-pairing an existing device refreshes it rather than duplicating.
	for (int i = 0; i < devices.size(); i++) {
		if (devices[i].id == id) {
			devices.write[i].name = p_name;
			devices.write[i].paired_at = now;
			devices.write[i].expires_at = now + DEFAULT_DEVICE_TTL_MS;
			save();
			Dictionary det;
			det["device_id"] = id;
			det["name"] = p_name;
			audit("device_repaired", det);
			return id;
		}
	}

	AIRemoteDevice dev;
	dev.id = id;
	dev.name = p_name;
	dev.public_key = p_public;
	dev.paired_at = now;
	dev.last_seen = now;
	dev.expires_at = now + DEFAULT_DEVICE_TTL_MS;
	dev.allow_control = false; // view-only until granted on the desktop
	devices.push_back(dev);
	save();

	Dictionary det;
	det["device_id"] = id;
	det["name"] = p_name;
	audit("device_paired", det);
	return id;
}

bool AIRemoteDeviceStore::remove_device(const String &p_id) {
	for (int i = 0; i < devices.size(); i++) {
		if (devices[i].id == p_id) {
			devices.remove_at(i);
			save();
			Dictionary det;
			det["device_id"] = p_id;
			audit("device_revoked", det);
			return true;
		}
	}
	return false;
}

void AIRemoteDeviceStore::remove_all() {
	devices.clear();
	save();
	audit("all_devices_revoked", Dictionary());
}

bool AIRemoteDeviceStore::set_allow_control(const String &p_id, bool p_allow) {
	for (int i = 0; i < devices.size(); i++) {
		if (devices[i].id == p_id) {
			devices.write[i].allow_control = p_allow;
			save();
			Dictionary det;
			det["device_id"] = p_id;
			det["allow_control"] = p_allow;
			audit("device_control_changed", det);
			return true;
		}
	}
	return false;
}

void AIRemoteDeviceStore::touch(const String &p_id, int64_t p_now_ms) {
	for (int i = 0; i < devices.size(); i++) {
		if (devices[i].id == p_id) {
			devices.write[i].last_seen = p_now_ms;
			return;
		}
	}
}

void AIRemoteDeviceStore::audit(const String &p_event, const Dictionary &p_details) {
	if (!DirAccess::exists(_dir())) {
		Ref<DirAccess> da = DirAccess::open("user://");
		if (da.is_null() || da->make_dir_recursive("ai_remote") != OK) {
			return;
		}
	}
	Dictionary row;
	row["ts"] = _now_ms();
	row["event"] = p_event;
	row["details"] = p_details;

	Ref<FileAccess> f = FileAccess::open("user://ai_remote/audit.jsonl", FileAccess::READ_WRITE);
	if (f.is_null()) {
		f = FileAccess::open("user://ai_remote/audit.jsonl", FileAccess::WRITE);
	}
	if (f.is_null()) {
		return;
	}
	f->seek_end();
	f->store_string(JSON::stringify(row) + "\n");
}
