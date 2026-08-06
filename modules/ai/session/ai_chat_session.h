/**************************************************************************/
/*  ai_chat_session.h                                                     */
/**************************************************************************/
/* Session seam between the AI chat state and any non-UI consumer         */
/* (remote clients, dashboards, tests).                                   */
/*                                                                        */
/* Owns the chat store and re-broadcasts everything a consumer needs to   */
/* mirror a live conversation. Commands are forwarded to an executor      */
/* registered by whoever actually drives the loop — today AIStatusPanel.  */
/* Nothing here includes a UI type, so a future headless extraction can   */
/* swap the executor without changing this API.                           */
/*                                                                        */
/* THREADING: main thread only. Consumers on other threads (the remote    */
/* server) must marshal through call_deferred.                            */
/**************************************************************************/

#ifndef AI_CHAT_SESSION_H
#define AI_CHAT_SESSION_H

#include "../editor/ai_chat_store.h"
#include "core/object/object.h"
#include "core/variant/callable.h"

class AIChatSession : public Object {
	GDCLASS(AIChatSession, Object);

	static AIChatSession *singleton;

	Ref<AIChatStore> store;

	// Executor callables, registered by the loop owner. Keys documented in
	// set_executor(). Missing keys make the matching command unavailable.
	Dictionary executor;

	// Mirrored run state, pushed by the executor.
	bool running = false;
	String status_text;
	Dictionary pending_approval;
	int policy_mode = 0;

	void _on_store_item_appended(int64_t p_ts, const Dictionary &p_data);
	void _on_store_items_rewritten();
	void _on_store_chat_changed(const String &p_chat_id);

	Error _invoke(const String &p_key, const Array &p_args);

protected:
	static void _bind_methods();

public:
	static AIChatSession *get_singleton() { return singleton; }

	// ---- Store ---------------------------------------------------------------
	// Created on first call; the panel adopts this instance rather than making
	// its own, so every existing chat_store-> call site keeps working unchanged.
	Ref<AIChatStore> get_store();

	// ---- Executor registration ----------------------------------------------
	// Keys (all optional, all Callable):
	//   "submit"      (String text)      -> send/queue a user message
	//   "cancel"      ()                 -> cancel the in-flight run
	//   "approval"    (String decision)  -> answer the pending approval
	//   "switch_chat" (String chat_id)   -> make chat_id active
	//   "new_chat"    ()                 -> start a new chat
	void set_executor(const Dictionary &p_handlers);
	void clear_executor();
	bool has_command(const String &p_key) const;

	// ---- Commands (any consumer) --------------------------------------------
	Error submit_user_message(const String &p_text);
	Error cancel_run();
	Error respond_approval(const String &p_decision);
	Error switch_chat(const String &p_chat_id);
	Error new_chat();

	// ---- State ---------------------------------------------------------------
	String get_chat_id() const;
	bool is_running() const { return running; }
	String get_status_text() const { return status_text; }
	Dictionary get_pending_approval() const { return pending_approval; }
	int get_policy_mode() const { return policy_mode; }

	// Full history as wire-ready dictionaries: [{ts, item}, ...]
	Array get_history() const;
	// Chat list, newest first: [{id, title, modified_ms, active}, ...]
	Array list_chats() const;
	// Cheap title for a chat: first user message, truncated. Reads only the
	// head of the file, so listing many chats stays inexpensive.
	static String make_chat_title(const String &p_chat_id);

	// ---- Tool screenshots -----------------------------------------------------
	// Persist tool-call screenshot(s) into the active chat's images folder and
	// return absolute OS path(s) ("" entries on failure or when no chat is
	// active). Lets tool executors put on-disk paths into the result the model
	// sees; the panel's later save of the same call id rewrites the same file,
	// so the two write paths stay consistent.
	String save_tool_screenshot(const String &p_call_id, const String &p_b64_png);
	Vector<String> save_tool_screenshots(const String &p_call_id, const Vector<String> &p_b64s);

	// ---- Broadcast entry points (called by the executor) ---------------------
	void notify_delta(const String &p_kind, const String &p_text); // "assistant" | "thinking"
	void notify_run_state(bool p_running);
	void notify_status(const String &p_text);
	void notify_approval(const Dictionary &p_info); // empty dict clears
	void notify_policy_mode(int p_mode);

	AIChatSession();
	~AIChatSession();
};

#endif // AI_CHAT_SESSION_H
