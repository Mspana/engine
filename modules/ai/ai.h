#ifndef AI_H
#define AI_H

#include "core/object/object.h"         // Base class
#include "core/io/image.h"              // For Ref<Image> in receive_screenshot
#include "core/variant/array.h"         // For Array return type
#include "core/string/ustring.h"        // For String parameter type
#include "core/variant/dictionary.h"    // Added for Dictionary type hint
#include "core/os/time.h"               // For Time singleton (journal timestamps)
#include "ai_journal_writer.h"          // For AIJournalWriter
#include "ai_provider.h"                // For AIProvider types
#include "retrieval.h"                  // For RetrievalIndex
#include "agentic_orchestrator.h"       // For agentic tool use

class AI : public Object {
	GDCLASS(AI, Object); // Godot class macro

	static AI *singleton; // Pointer to the singleton instance

protected:
	// Binds methods to be used in scripting.
	static void _bind_methods();

	// Helper method for binding validate_command_json to GDScript.
	Dictionary _validate_command_json_bind(const String &json_str) const;

	// Validates the JSON string; on failure, returns false and populates error_msg.
    bool validate_command_json(const String &json_str, String &error_msg) const;

private:
	// Provider for AI API calls
	Ref<AIProvider> provider;

	// Retrieval index for RAG-lite context
	Ref<RetrievalIndex> retrieval;

	// Agentic orchestrator for multi-turn tool use
	Ref<AgenticOrchestrator> orchestrator;

	// Callback for provider request completion
	void _on_provider_request_completed(bool success, const String &response_json, const String &error_message);

	// Agentic callbacks
	void _on_agentic_run_started();
	void _on_agentic_tool_result(const Dictionary &p_tool_result);
	void _on_agentic_progress(const String &p_status, int p_turn);
	void _on_agentic_complete(bool p_success, const String &p_final_message);

	// Game session context storage (populated on debugger stop, cleared after AI consumes it)
	String _last_session_screenshot_b64;
	bool _session_screenshot_pending = false;

	void _connect_debugger_signals();
	void _on_game_session_stopped();

	// Journal
	AIJournalWriter *_journal_writer = nullptr;
	Array _run_action_buffer;   // Cleared at run start, accumulates _tool_result_data dicts
	uint64_t _run_start_ms = 0; // Ticks at run_started
	String _current_run_id;     // Timestamp-based ID, set at run_started

	Dictionary _exec_write_dev_note(const Dictionary &args);
	Dictionary _exec_update_todos(const Dictionary &args);
	String _get_journal_path(const String &p_filename) const;

	// Helper to process and execute actions from JSON response
	void _process_and_execute_actions(const String &ai_json_response);

	// Execute single action and return result (for agentic orchestrator)
	Dictionary _execute_single_action_internal(const Dictionary &p_action);

	// Helper to validate a command already parsed into a Dictionary.
	bool _validate_command_dictionary(const Dictionary &cmd, String &error_msg) const;

	// Helper to get active scene path
	String _get_active_scene_path() const;

	// Check conversation history for a prior read_script call on this file
	bool _was_file_read_in_history(const String &file_path) const;

	// File operation helpers for UndoRedo
	void _create_script_file(const String &abs_path, const String &content);
	void _write_script_file(const String &abs_path, const String &content);
	void _delete_script_file(const String &abs_path);
	void _rename_script_file(const String &old_abs_path, const String &new_abs_path);
	void _write_binary_file(const String &abs_path, const PackedByteArray &bytes);

public:
	// The core method to interact with the AI backend (single message, legacy).
	Array request_actions(const String &prompt);

	// Request with full conversation history
	Array request_actions_with_history(const Array &p_messages);

	// Execute a single action and return structured result (for orchestrator)
	Dictionary execute_single_action(const Dictionary &p_action);

	// Get the orchestrator (for UI to access cancel, status, etc.)
	Ref<AgenticOrchestrator> get_orchestrator() const;

	// Provider management
	void set_provider(const Ref<AIProvider> &p_provider);
	Ref<AIProvider> get_provider() const;

	// Static methods for singleton management (called from register_types.cpp)
	static void initialize_singleton();
	static void finalize_singleton();
	// Static getter for easy C++ access to the singleton.
	static AI *get_singleton();

	// Screenshot routing: called by game_view_plugin, emits screenshot_for_chat signal
	void receive_screenshot(Ref<Image> p_image);

	// Game screenshot signal chain (for run_and_screenshot action, no cross-module deps)
	void trigger_game_screenshot();                        // emits game_screenshot_requested
	void deliver_game_screenshot(const String &p_b64);     // called by GameView, emits game_screenshot_ready
	bool get_game_is_running() const;                      // returns true if game is currently playing
	void stop_game();                                      // stops the running game

	// Game session context (auto-captured on stop, injected into next AI turn)
	Dictionary consume_session_context();                  // returns and clears stored session data

	// Check if a file was read via read_script in the current conversation
	bool was_file_read(const String &file_path) const;

	// Constructor and Destructor
	AI();
	~AI();
};

#endif // AI_H 