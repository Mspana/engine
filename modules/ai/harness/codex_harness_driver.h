/**************************************************************************/
/*  codex_harness_driver.h                                                */
/**************************************************************************/
/* Phase 2 of the harness replacement (docs/design/harness_replacement_   */
/* plan.md): drives a vendored `codex app-server` child process over      */
/* JSONL/stdio and re-emits the AgenticOrchestrator signal contract so    */
/* AIStatusPanel can swap loops without changes.                          */
/*                                                                        */
/* Increment 1 (smoke-able without UI): spawn + initialize handshake +    */
/* apiKey login + thread/start with dynamicTools built from               */
/* AIProvider::build_tools_array() + turn lifecycle + item/tool/call      */
/* execution through AI::execute_single_action + interrupt/steer.         */
/**************************************************************************/

#pragma once

#include "core/io/file_access.h"
#include "core/object/ref_counted.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/safe_refcount.h"

class CodexHarnessDriver : public RefCounted {
	GDCLASS(CodexHarnessDriver, RefCounted);

public:
	// Spawns codex app-server and begins the session handshake. Safe to call
	// once; returns false if the binary could not be spawned.
	bool start_session();
	void shutdown();

	// One user turn. Emits the orchestrator signal set as events stream back.
	void send_user_message(const String &p_text, int64_t p_user_message_id);
	// Mid-run steering (turn/steer) — appends input to the in-flight turn.
	void steer(const String &p_text);
	// Instant cancel (turn/interrupt + generation guard).
	void cancel_run();

	bool is_running() const { return turn_active; }
	bool is_session_ready() const { return session_ready; }

	// Session continuity: when set before start_session(), the driver resumes
	// the existing codex thread (history intact) instead of starting fresh.
	void set_resume_thread_id(const String &p_id) { resume_thread_id = p_id; }
	String get_thread_id() const { return thread_id; }

	// Approval policy (Shift+Tab cycled in the panel). Codex always runs with
	// approvalPolicy "untrusted"; the DRIVER is the policy engine and decides
	// per mode. Protected editor files are declined in every mode.
	enum PolicyMode {
		POLICY_ASK = 0, // surface an approval card, user decides
		POLICY_AUTO = 1, // auto-accept commands/patches (protected files still declined)
		POLICY_READ_ONLY = 2, // read-only sandbox; all writes declined
	};
	void set_policy_mode(int p_mode) { policy_mode = (PolicyMode)CLAMP(p_mode, 0, 2); }
	int get_policy_mode() const { return policy_mode; }
	// Answers a pending approval ("accept" / "acceptForSession" / "decline").
	void respond_approval(int p_request_id, const String &p_decision);

protected:
	static void _bind_methods();

private:
	enum SessionPhase {
		PHASE_IDLE,
		PHASE_INITIALIZING,
		PHASE_CHECKING_ACCOUNT,
		PHASE_LOGGING_IN,
		PHASE_STARTING_THREAD,
		PHASE_READY,
	};

	// Child process. Pipes are BLOCKING: writes always complete in full (a
	// non-blocking Windows pipe truncates frames larger than its 4 KB buffer),
	// and blocking reads return as soon as bytes arrive, each on its own thread.
	Ref<FileAccess> stdio;
	Ref<FileAccess> stderr_pipe;
	OS::ProcessID child_pid = 0;
	Thread reader_thread;
	Thread stderr_thread;
	SafeFlag reader_exit;
	SafeFlag child_alive;

	// JSON-RPC state (main thread only, except next_id).
	int next_request_id = 0;
	SessionPhase phase = PHASE_IDLE;
	bool session_ready = false;
	String thread_id;
	String resume_thread_id;
	String current_turn_id;
	bool turn_active = false;
	int turn_counter = 0;
	uint64_t run_generation = 0; // Stale-event guard across cancels.
	int64_t active_user_message_id = 0;
	String assistant_text_accum;
	String queued_message; // Message sent before the session became ready.
	int64_t queued_message_id = 0;
	bool smoke_mode = false; // Verbose event prints for headless validation.

	// Request-id bookkeeping for the bring-up state machine and turn starts.
	int pending_phase_request = -1;
	int pending_turn_request = -1;

	// Approval routing. pending_approvals values:
	//   {type:"native"} — codex shell/patch request; answer with {decision}.
	//   {type:"editor_tool", params} — a gated dynamic tool call held BEFORE
	//   execution; on accept it executes and the tool response is sent.
	PolicyMode policy_mode = POLICY_ASK;
	HashMap<int, Dictionary> pending_approvals;
	HashSet<String> session_allowed_tools; // "Allow for session" cache (editor tools)
	HashMap<String, Dictionary> file_change_items; // recent fileChange items by id (approval display + guard)
	void _route_approval(int p_request_id, const Dictionary &p_info);
	void _decline_pending_approvals();
	void _refuse_tool_call(int p_request_id, const String &p_tool, const String &p_call_id, const String &p_message, const String &p_status);
	static bool _is_protected_path(const String &p_path);
	static bool _is_read_only_tool(const String &p_tool);

	static void _reader_thread_func(void *p_userdata);
	static void _stderr_thread_func(void *p_userdata);
	void _reader_loop();
	void _stderr_loop();

	// All main-thread (dispatched via call_deferred from the reader).
	void _handle_frame(const String &p_frame);
	void _handle_response(int p_id, const Dictionary &p_result, const Dictionary &p_error);
	void _handle_notification(const String &p_method, const Dictionary &p_params);
	void _handle_server_request(int p_id, const String &p_method, const Dictionary &p_params);
	void _handle_child_exit();

	void _advance_phase(const Dictionary &p_result);
	void _start_thread_request();
	void _start_turn(const String &p_text);
	Dictionary _execute_dynamic_tool(const Dictionary &p_params);
	Array _build_dynamic_tools();
	String _developer_instructions();

	// Hidden context injection (ported orchestrator formats).
	String _build_game_session_context();
	String _build_user_scene_changes();
	String _take_ai_scene_update();

	// --- run_and_screenshot: held-open dynamic tool call. The JSON-RPC
	// response to codex's item/tool/call is deferred until the game has
	// launched and every requested capture has arrived (ported from the
	// orchestrator's timer state machine). ---
	enum AsyncRnsPhase {
		RNS_INACTIVE,
		RNS_POLL_START,
		RNS_WAIT_VISUAL,
		RNS_AWAIT_CAPTURE,
	};
	AsyncRnsPhase rns_phase = RNS_INACTIVE;
	int rns_request_id = -1; // codex's item/tool/call id, answered at the end
	String rns_call_id;
	Dictionary rns_args;
	Vector<float> rns_capture_times;
	int rns_next_capture_index = 0;
	Array rns_captured;
	uint64_t rns_phase_start_ms = 0;
	uint64_t rns_action_start_ms = 0;
	uint64_t rns_game_running_ms = 0;
	uint32_t rns_tick_gen = 0;
	uint64_t rns_run_generation = 0;

	void _begin_async_rns(int p_request_id, const Dictionary &p_params);
	void _schedule_rns_tick(float p_delay);
	void _rns_tick_gen_cb(uint32_t p_gen);
	void _rns_tick();
	void _on_rns_capture_received(const String &p_b64);
	void _finish_async_rns(const Dictionary &p_exec_result);
	void _abort_async_rns(const String &p_reason);
	Dictionary _tool_response_from_result(const String &p_tool, const Dictionary &p_args, const String &p_call_id, const Dictionary &p_exec_result);

	int _send_request(const String &p_method, const Dictionary &p_params);
	void _send_notification(const String &p_method, const Dictionary &p_params);
	void _send_response(int p_id, const Dictionary &p_result);
	void _write_frame(const Dictionary &p_frame);

	void _smoke(const String &p_line);
};
