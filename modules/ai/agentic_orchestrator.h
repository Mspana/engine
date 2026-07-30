/**************************************************************************/
/*  agentic_orchestrator.h                                                */
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

#ifndef AGENTIC_ORCHESTRATOR_H
#define AGENTIC_ORCHESTRATOR_H

#include "core/object/ref_counted.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

// Forward declaration
class AIProvider;

/**
 * AgenticOrchestrator - Manages multi-turn agentic loops for AI tool use
 *
 * This orchestrator enables the AI to:
 * 1. Execute actions
 * 2. Receive structured tool results
 * 3. Repair errors and iterate
 * 4. Provide a final response
 *
 * All within a single user message request.
 */
class AgenticOrchestrator : public RefCounted {
	GDCLASS(AgenticOrchestrator, RefCounted);

public:
	// Guardrail constants
	static constexpr int MAX_MODEL_TURNS_PER_RUN = 50;
	static constexpr int MAX_ACTIONS_PER_RESPONSE = 12;
	static constexpr int MAX_ACTIONS_PER_RUN = 200;
	// Max automatic retries for a single model request after a transient network
	// failure (connection reset, DNS/TLS, timeout). Retries use exponential backoff
	// and do not consume a model turn.
	static constexpr int MAX_REQUEST_RETRIES = 3;
	// MAX_REPAIR_CYCLES removed — native tool-calling handles validation via the API

	// Per-item in the AI's self-managed task list
	struct TodoItem {
		String id;
		String content;
		String status; // "pending" | "in_progress" | "completed"
	};

	// Run state
	struct RunContext {
		Array conversation_history; // Full chat transcript
		Array run_messages; // Messages added during this run (tool results)
		int model_turns = 0;
		int total_actions = 0;
		// repair_cycles removed — native tool-calling
		bool cancelled = false;
		String user_message; // Original user message for context
		int64_t user_message_id = 0; // Message ID for checkpoint anchoring
		Vector<TodoItem> todos; // AI's self-managed task list (run-scoped)
		bool has_todos = false;
	};

	AgenticOrchestrator();
	~AgenticOrchestrator();

	// Main entry point - runs the agentic loop
	void run_agentic_loop(const Array &p_initial_messages, Ref<AIProvider> p_provider);

	// Cancel control. cancel_run() is terminal and instant: it aborts the
	// in-flight request (or tears down an async run_and_screenshot), repairs
	// the transcript, and emits run_complete before returning — the caller
	// never waits on network or tools.
	void cancel_run();
	bool is_cancelled() const;
	bool is_running() const;

	// Set the user message ID for checkpoint anchoring (call after run_agentic_loop)
	void set_user_message_id(int64_t p_user_message_id);

	// Update the AI's self-managed task list (called from _exec_update_todos)
	void set_todos(const Array &p_todos);

	// Get current run stats
	int get_model_turns() const;
	int get_total_actions() const;
	int get_repair_cycles() const;
	String get_user_message() const;
	int64_t get_user_message_id() const;

	// Get conversation history (for validation checks like read-before-write)
	Array get_conversation_history() const;

	// Inject a user message mid-run (appended before the next model turn).
	// p_id lets the UI track the message until it is consumed: consumption is
	// announced via the user_injection_consumed signal, and an unconsumed
	// message can be withdrawn with remove_pending_injection.
	bool inject_user_message(const String &p_id, const String &p_message);
	void remove_pending_injection(const String &p_id);

protected:
	static void _bind_methods();

private:
	RunContext current_run;
	bool _is_running = false;
	bool _waiting_for_response = false;
	Ref<AIProvider> provider;

	// Retry count for the in-flight model request; reset on success and at run start.
	int _request_retry_attempt = 0;

	// Bumped at every run start. Bound into deferred callbacks that must not
	// fire into a later run (e.g. the transient-failure retry timer): with
	// instant cancel, a run can end and a new one start while such a timer is
	// still pending.
	uint64_t _run_gen = 0;

	// Pending user injections (mid-run messages, consumed before next API call).
	struct PendingInjection {
		String id;
		String text;
	};
	Vector<PendingInjection> _pending_user_injections;

	// Pending response for deferred processing (avoids ProgressDialog issues)
	Dictionary _pending_response;

	// Tool calls from the current assistant response (needed for synthetic cancel)
	Array _current_tool_calls;

	// Async provider callback
	void _on_provider_response(bool p_success, const String &p_response, const String &p_error);

	// Request sending (initiates async call)
	void _send_model_request();

	// Transient network-failure retry. Classifies pre-response transport failures
	// (vs. real API errors), then re-sends the same request after a backoff delay
	// without consuming a model turn.
	static bool _is_transient_network_error(const String &p_error);
	void _schedule_request_retry(float p_delay_seconds);
	void _retry_model_request(uint64_t p_run_gen);

	// Response processing (deferred to next frame to avoid message queue conflicts)
	void _process_model_response_deferred();

	// Native tool-calling response processing
	void _process_native_tool_response(const Dictionary &p_api_response);

	// Execute a single tool call and return the result as a tool message
	Dictionary _execute_tool_call(const String &p_call_id, const String &p_tool_name, const Dictionary &p_args);

	// Guardrail handlers
	void _handle_cancellation();
	void _handle_max_turns_exceeded();
	void _handle_max_actions_exceeded();

	// Cancel-time transcript repair: append a synthetic cancelled result for
	// one tool call, and for every call in _current_tool_calls that has no
	// tool response in conversation_history yet (no-orphan invariant).
	void _append_cancelled_tool_result(const Dictionary &p_tool_call);
	void _synthesize_cancelled_results_for_unanswered();

	// Scene diffs: after a tool batch, append a [SCENE UPDATE] message showing
	// how the batch changed each touched scene's serialized .tscn text.
	// At run start, run_agentic_loop injects [SCENE CHANGES] for edits made
	// outside the conversation (user edits between runs).
	void _append_batch_scene_diffs();
	// Refresh snapshots without emitting messages (cancelled batches), so the
	// next run's user-attributed diff doesn't pick up the AI's own changes.
	void _refresh_batch_snapshots_silent();
	// Shared formatter: one scene's entry for a diff context block.

	// Signal emissions
	void _emit_progress_update(const String &p_status, int p_turn);
	void _emit_tool_result(const Dictionary &p_tool_result);
	void _emit_run_complete(bool p_success, const String &p_final_message);

	int _current_turn_tokens = 0; // total_tokens from the most recent API response usage field

	// Async run_and_screenshot state (timer-based, never blocks main thread)
	enum AsyncRnsPhase { ASYNC_RNS_INACTIVE, ASYNC_RNS_POLL_START, ASYNC_RNS_WAIT_VISUAL, ASYNC_RNS_AWAIT_CAPTURE };
	AsyncRnsPhase _async_rns_phase = ASYNC_RNS_INACTIVE;
	String _async_rns_tool_call_id; // tool_call_id for native format
	Dictionary _async_rns_action_args;
	uint64_t _async_rns_phase_start_ms = 0;
	uint64_t _async_rns_action_start_ms = 0; // Set once at action entry; never reset on phase transitions.
	uint32_t _rns_tick_gen = 0;

	// Multi-screenshot support. `_async_rns_capture_times` is sorted ascending and
	// holds every requested capture time (seconds, relative to the moment the game
	// is first observed running). `_async_rns_next_capture_index` walks through it.
	// `_async_rns_captured_b64s` accumulates `{at_seconds, b64}` dicts as each
	// capture lands. When `next_capture_index == capture_times.size()` the game
	// is stopped and the orchestrator finalises with all collected screenshots.
	Vector<float> _async_rns_capture_times;
	int _async_rns_next_capture_index = 0;
	Array _async_rns_captured_b64s;
	uint64_t _async_rns_game_running_ms = 0; // Set when game first reports running (POLL_START → WAIT_VISUAL).

	void _schedule_rns_tick(float p_delay = 0.05f);
	void _run_and_screenshot_tick_gen(uint32_t p_gen);
	void _run_and_screenshot_tick();
	void _on_async_rns_capture_received(const String &p_b64);
	void _on_async_rns_complete(const Dictionary &p_exec_result);

	// Async install_export_templates state (second instance of the RNS
	// timer-tick pattern above; see export_actions.h for the download URL).
	// Download runs on a threaded HTTPRequest node parented under EditorNode;
	// extraction runs on a WorkerThreadPool task owned by a heap context the
	// tick polls (so cancel never blocks on the worker).
	enum AsyncTplPhase { ASYNC_TPL_INACTIVE, ASYNC_TPL_DOWNLOADING, ASYNC_TPL_EXTRACTING };
	AsyncTplPhase _async_tpl_phase = ASYNC_TPL_INACTIVE;
	String _async_tpl_tool_call_id;
	Dictionary _async_tpl_action_args;
	uint64_t _async_tpl_start_ms = 0;
	uint64_t _async_tpl_last_progress_ms = 0; // Download stall watchdog.
	int64_t _async_tpl_last_bytes = 0;
	uint32_t _tpl_tick_gen = 0;
	ObjectID _async_tpl_http_id; // HTTPRequest node (lives under EditorNode).
	String _async_tpl_tmp_path; // Downloaded .tpz in the editor temp dir.
	struct AITplExtractContext *_async_tpl_extract_ctx = nullptr;

	// export_project / serve_web_build drive EditorProgress internally, which
	// refuses to start while the message queue is flushing — and tool dispatch
	// runs from call_deferred. These tools are re-entered from a SceneTreeTimer
	// callback (outside the flush) and executed synchronously there.
	String _deferred_tool_call_id;
	String _deferred_tool_name;
	Dictionary _deferred_tool_args;
	void _run_deferred_sync_tool(uint64_t p_run_gen);

	void _tpl_release_extract_ctx(bool p_cancel);
	void _schedule_tpl_tick(float p_delay = 0.5f);
	void _install_templates_tick_gen(uint32_t p_gen);
	void _install_templates_tick();
	void _on_tpl_download_completed(int p_result, int p_response_code, const PackedStringArray &p_headers, const PackedByteArray &p_body);
	void _on_async_tpl_complete(const Dictionary &p_exec_result);
};

#endif // AGENTIC_ORCHESTRATOR_H
