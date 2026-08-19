// modules/ai/runtime_error_log.h
// Runtime error log for game runs (script_execution_plan.md §5 Phase C).
//
// Streams the running game's errors/warnings into a stable JSONL file
// (user://ai_journal/runtime_errors.jsonl) as they arrive from the debugger,
// brackets every run with run_start / run_end markers, and records crashes
// with the exact output tail and exit code — a crash must never look like a
// quiet run. The run tools push a small digest of this state into their
// results; the agent pulls the file itself when the digest is not enough.

#ifndef AI_RUNTIME_ERROR_LOG_H
#define AI_RUNTIME_ERROR_LOG_H

#include "core/object/object.h"
#include "core/templates/hash_set.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class AIJournalWriter;

class AIRuntimeErrorLog : public Object {
	GDCLASS(AIRuntimeErrorLog, Object);

protected:
	static void _bind_methods() {}

private:
	AIJournalWriter *_writer = nullptr; // Owned by AI, outlives this object.
	String _log_path_abs;               // Absolute path (user:// resolved by AI).

	bool _wired = false;

	// Current / last run state. Reset at run_start, finalized at run_end.
	bool _run_active = false;
	String _run_id;
	uint64_t _run_start_ticks_ms = 0;
	int _error_count = 0;
	int _warning_count = 0;
	HashSet<String> _unique_error_keys;
	Array _first_errors; // Up to FIRST_ERRORS_MAX verbatim unique error strings.

	// Finalized at run_end.
	String _exit_status; // "stopped" | "exited" | "crashed"
	bool _has_exit_code = false;
	int _exit_code = 0;
	uint64_t _duration_ms = 0;

	// Set by the run tools just before launching, consumed by _on_play_pressed
	// so agent-initiated runs are marked as such (with the requested mode).
	String _pending_mode;
	String _pending_scene;
	uint64_t _pending_ticks_ms = 0;

	static const int FIRST_ERRORS_MAX = 5;
	static const uint64_t ROTATE_BYTES = 4 * 1024 * 1024;

	void _enqueue(const Dictionary &p_entry);
	void _maybe_rotate();
	void _write_run_end(const String &p_exit_status, bool p_has_exit_code, int p_exit_code);

	void _on_play_pressed();
	void _on_stop_pressed();
	void _on_runtime_error(const Dictionary &p_error);

public:
	// The user://-form path pushed to the model; _log_path_abs is its resolution.
	static String get_log_path_user() { return "user://ai_journal/runtime_errors.jsonl"; }

	// Idempotent; connects to EditorRunBar and the default debugger session.
	// Safe to call before the editor is fully up (retries on the next call).
	void ensure_wired();

	// Called by the run tools before launching, so the run_start marker records
	// the agent as initiator along with the requested mode/scene.
	void note_run_requested(const String &p_scene, const String &p_mode);

	// Digest of the current (running=true) or most recently ended run. Empty
	// Dictionary when no run has been observed since editor start.
	Dictionary get_run_digest() const;

	AIRuntimeErrorLog(AIJournalWriter *p_writer, const String &p_log_path_abs);
};

#endif // AI_RUNTIME_ERROR_LOG_H
