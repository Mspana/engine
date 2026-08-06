/**************************************************************************/
/*  ai_chat_session.cpp                                                   */
/**************************************************************************/

#include "ai_chat_session.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/variant/callable.h"

AIChatSession *AIChatSession::singleton = nullptr;

AIChatSession::AIChatSession() {
	singleton = this;
}

AIChatSession::~AIChatSession() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

void AIChatSession::_bind_methods() {
	ClassDB::bind_method(D_METHOD("submit_user_message", "text"), &AIChatSession::submit_user_message);
	ClassDB::bind_method(D_METHOD("cancel_run"), &AIChatSession::cancel_run);
	ClassDB::bind_method(D_METHOD("respond_approval", "decision"), &AIChatSession::respond_approval);
	ClassDB::bind_method(D_METHOD("switch_chat", "chat_id"), &AIChatSession::switch_chat);
	ClassDB::bind_method(D_METHOD("new_chat"), &AIChatSession::new_chat);
	ClassDB::bind_method(D_METHOD("set_executor", "handlers"), &AIChatSession::set_executor);
	ClassDB::bind_method(D_METHOD("clear_executor"), &AIChatSession::clear_executor);
	ClassDB::bind_method(D_METHOD("has_command", "key"), &AIChatSession::has_command);
	ClassDB::bind_method(D_METHOD("get_chat_id"), &AIChatSession::get_chat_id);
	ClassDB::bind_method(D_METHOD("is_running"), &AIChatSession::is_running);
	ClassDB::bind_method(D_METHOD("get_history"), &AIChatSession::get_history);
	ClassDB::bind_method(D_METHOD("list_chats"), &AIChatSession::list_chats);
	ClassDB::bind_method(D_METHOD("get_pending_approval"), &AIChatSession::get_pending_approval);
	ClassDB::bind_method(D_METHOD("get_policy_mode"), &AIChatSession::get_policy_mode);
	ClassDB::bind_method(D_METHOD("get_status_text"), &AIChatSession::get_status_text);
	ClassDB::bind_method(D_METHOD("notify_delta", "kind", "text"), &AIChatSession::notify_delta);
	ClassDB::bind_method(D_METHOD("notify_run_state", "running"), &AIChatSession::notify_run_state);
	ClassDB::bind_method(D_METHOD("notify_status", "text"), &AIChatSession::notify_status);
	ClassDB::bind_method(D_METHOD("notify_approval", "info"), &AIChatSession::notify_approval);
	ClassDB::bind_method(D_METHOD("notify_policy_mode", "mode"), &AIChatSession::notify_policy_mode);

	// Internal store-signal relays.
	ClassDB::bind_method(D_METHOD("_on_store_item_appended", "ts", "data"), &AIChatSession::_on_store_item_appended);
	ClassDB::bind_method(D_METHOD("_on_store_items_rewritten"), &AIChatSession::_on_store_items_rewritten);
	ClassDB::bind_method(D_METHOD("_on_store_chat_changed", "chat_id"), &AIChatSession::_on_store_chat_changed);

	ADD_SIGNAL(MethodInfo("item_appended", PropertyInfo(Variant::INT, "ts"), PropertyInfo(Variant::DICTIONARY, "data")));
	ADD_SIGNAL(MethodInfo("history_changed", PropertyInfo(Variant::STRING, "reason")));
	ADD_SIGNAL(MethodInfo("chat_switched", PropertyInfo(Variant::STRING, "chat_id")));
	ADD_SIGNAL(MethodInfo("delta", PropertyInfo(Variant::STRING, "kind"), PropertyInfo(Variant::STRING, "text")));
	ADD_SIGNAL(MethodInfo("run_state_changed", PropertyInfo(Variant::BOOL, "running")));
	ADD_SIGNAL(MethodInfo("status_changed", PropertyInfo(Variant::STRING, "text")));
	ADD_SIGNAL(MethodInfo("approval_changed", PropertyInfo(Variant::DICTIONARY, "info")));
	ADD_SIGNAL(MethodInfo("policy_mode_changed", PropertyInfo(Variant::INT, "mode")));
}

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

Ref<AIChatStore> AIChatSession::get_store() {
	if (store.is_null()) {
		// One sweep per editor session: drop image folders / meta files whose
		// chat transcript no longer exists.
		AIChatStore::cleanup_orphaned_chat_files();
		store.instantiate();
		store->connect("item_appended", callable_mp(this, &AIChatSession::_on_store_item_appended));
		store->connect("items_rewritten", callable_mp(this, &AIChatSession::_on_store_items_rewritten));
		store->connect("chat_changed", callable_mp(this, &AIChatSession::_on_store_chat_changed));
	}
	return store;
}

String AIChatSession::save_tool_screenshot(const String &p_call_id, const String &p_b64_png) {
	Ref<AIChatStore> s = get_store();
	if (s.is_null() || s->get_chat_id().is_empty() || p_call_id.is_empty() || p_b64_png.is_empty()) {
		return String();
	}
	String filename = s->save_screenshot(p_call_id, p_b64_png);
	if (filename.is_empty()) {
		return String();
	}
	return ProjectSettings::get_singleton()->globalize_path(s->get_images_dir().path_join(filename));
}

Vector<String> AIChatSession::save_tool_screenshots(const String &p_call_id, const Vector<String> &p_b64s) {
	Vector<String> paths;
	paths.resize(p_b64s.size());
	Ref<AIChatStore> s = get_store();
	if (s.is_null() || s->get_chat_id().is_empty() || p_call_id.is_empty()) {
		return paths;
	}
	Vector<String> filenames = s->save_screenshots(p_call_id, p_b64s);
	for (int i = 0; i < filenames.size() && i < paths.size(); i++) {
		if (!filenames[i].is_empty()) {
			paths.write[i] = ProjectSettings::get_singleton()->globalize_path(s->get_images_dir().path_join(filenames[i]));
		}
	}
	return paths;
}

void AIChatSession::_on_store_item_appended(int64_t p_ts, const Dictionary &p_data) {
	emit_signal(SNAME("item_appended"), p_ts, p_data);
	emit_signal(SNAME("history_changed"), "append");
}

void AIChatSession::_on_store_items_rewritten() {
	emit_signal(SNAME("history_changed"), "rewrite");
}

void AIChatSession::_on_store_chat_changed(const String &p_chat_id) {
	emit_signal(SNAME("chat_switched"), p_chat_id);
	emit_signal(SNAME("history_changed"), "switch");
}

// ---------------------------------------------------------------------------
// Executor
// ---------------------------------------------------------------------------

void AIChatSession::set_executor(const Dictionary &p_handlers) {
	executor = p_handlers;
}

void AIChatSession::clear_executor() {
	executor.clear();
}

bool AIChatSession::has_command(const String &p_key) const {
	if (!executor.has(p_key)) {
		return false;
	}
	Variant v = executor[p_key];
	return v.get_type() == Variant::CALLABLE && ((Callable)v).is_valid();
}

Error AIChatSession::_invoke(const String &p_key, const Array &p_args) {
	if (!has_command(p_key)) {
		return ERR_UNAVAILABLE;
	}
	Callable cb = executor[p_key];
	Callable::CallError ce;
	Variant ret;

	LocalVector<Variant> argv;
	LocalVector<const Variant *> argp;
	argv.resize(p_args.size());
	argp.resize(p_args.size());
	for (int i = 0; i < p_args.size(); i++) {
		argv[i] = p_args[i];
		argp[i] = &argv[i];
	}

	cb.callp(argp.ptr(), p_args.size(), ret, ce);
	if (ce.error != Callable::CallError::CALL_OK) {
		ERR_PRINT(vformat("AIChatSession: executor '%s' call failed (error %d).", p_key, (int)ce.error));
		return FAILED;
	}
	return OK;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

Error AIChatSession::submit_user_message(const String &p_text) {
	if (p_text.strip_edges().is_empty()) {
		return ERR_INVALID_PARAMETER;
	}
	Array args;
	args.push_back(p_text);
	return _invoke("submit", args);
}

Error AIChatSession::cancel_run() {
	return _invoke("cancel", Array());
}

Error AIChatSession::respond_approval(const String &p_decision) {
	if (pending_approval.is_empty()) {
		return ERR_UNAVAILABLE;
	}
	// Only the three decisions the driver understands.
	if (p_decision != "accept" && p_decision != "acceptForSession" && p_decision != "decline") {
		return ERR_INVALID_PARAMETER;
	}
	Array args;
	args.push_back(p_decision);
	return _invoke("approval", args);
}

Error AIChatSession::switch_chat(const String &p_chat_id) {
	if (!p_chat_id.begins_with("chat_")) {
		return ERR_INVALID_PARAMETER;
	}
	Array args;
	args.push_back(p_chat_id);
	return _invoke("switch_chat", args);
}

Error AIChatSession::new_chat() {
	return _invoke("new_chat", Array());
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

String AIChatSession::get_chat_id() const {
	return store.is_valid() ? store->get_chat_id() : String();
}

Array AIChatSession::get_history() const {
	Array out;
	if (store.is_null()) {
		return out;
	}
	const Vector<HistoryItem> &items = store->get_items();
	for (int i = 0; i < items.size(); i++) {
		Dictionary row;
		row["ts"] = items[i].ts;
		row["item"] = items[i].data;
		out.push_back(row);
	}
	return out;
}

String AIChatSession::make_chat_title(const String &p_chat_id) {
	Ref<FileAccess> f = FileAccess::open(AIChatStore::make_chat_path(p_chat_id), FileAccess::READ);
	if (f.is_null()) {
		return p_chat_id;
	}
	// Scan a bounded number of lines for the first user message.
	for (int i = 0; i < 40 && !f->eof_reached(); i++) {
		String line = f->get_line().strip_edges();
		if (line.is_empty()) {
			continue;
		}
		JSON json;
		if (json.parse(line) != OK || json.get_data().get_type() != Variant::DICTIONARY) {
			continue;
		}
		Dictionary row = json.get_data();
		if (!row.has("item") || row["item"].get_type() != Variant::DICTIONARY) {
			continue;
		}
		Dictionary item = row["item"];
		if (String(item.get("role", "")) != "user") {
			continue;
		}
		String content = item.get("content", "");
		content = content.replace("\n", " ").strip_edges();
		if (content.is_empty()) {
			continue;
		}
		return content.length() > 60 ? content.substr(0, 60) + "..." : content;
	}
	return p_chat_id;
}

Array AIChatSession::list_chats() const {
	Array out;
	Vector<String> ids = AIChatStore::list_chat_ids();
	String active = get_chat_id();
	for (int i = 0; i < ids.size(); i++) {
		Dictionary row;
		row["id"] = ids[i];
		row["title"] = make_chat_title(ids[i]);
		row["modified_ms"] = (int64_t)FileAccess::get_modified_time(AIChatStore::make_chat_path(ids[i])) * 1000;
		row["active"] = (ids[i] == active);
		out.push_back(row);
	}
	return out;
}

// ---------------------------------------------------------------------------
// Broadcast entry points
// ---------------------------------------------------------------------------

void AIChatSession::notify_delta(const String &p_kind, const String &p_text) {
	if (p_text.is_empty()) {
		return;
	}
	emit_signal(SNAME("delta"), p_kind, p_text);
}

void AIChatSession::notify_run_state(bool p_running) {
	if (running == p_running) {
		return;
	}
	running = p_running;
	emit_signal(SNAME("run_state_changed"), running);
}

void AIChatSession::notify_status(const String &p_text) {
	if (status_text == p_text) {
		return;
	}
	status_text = p_text;
	emit_signal(SNAME("status_changed"), status_text);
}

void AIChatSession::notify_approval(const Dictionary &p_info) {
	pending_approval = p_info;
	emit_signal(SNAME("approval_changed"), pending_approval);
}

void AIChatSession::notify_policy_mode(int p_mode) {
	if (policy_mode == p_mode) {
		return;
	}
	policy_mode = p_mode;
	emit_signal(SNAME("policy_mode_changed"), policy_mode);
}
