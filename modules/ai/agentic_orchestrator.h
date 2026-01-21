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
	static constexpr int MAX_MODEL_TURNS_PER_RUN = 6;
	static constexpr int MAX_ACTIONS_PER_RUN = 25;
	static constexpr int MAX_REPAIR_CYCLES = 2;

	// Run state
	struct RunContext {
		Array conversation_history; // Full chat transcript
		Array run_messages; // Messages added during this run (tool results)
		int model_turns = 0;
		int total_actions = 0;
		int repair_cycles = 0;
		bool cancelled = false;
		String user_message; // Original user message for context
	};

	AgenticOrchestrator();
	~AgenticOrchestrator();

	// Main entry point - runs the agentic loop
	void run_agentic_loop(const Array &p_initial_messages, Ref<AIProvider> p_provider);

	// Cancel control
	void cancel_run();
	bool is_cancelled() const;
	bool is_running() const;

	// Get current run stats
	int get_model_turns() const;
	int get_total_actions() const;

protected:
	static void _bind_methods();

private:
	RunContext current_run;
	bool _is_running = false;
	bool _waiting_for_response = false;
	Ref<AIProvider> provider;

	// Async provider callback
	void _on_provider_response(bool p_success, const String &p_response, const String &p_error);

	// Request sending (initiates async call)
	void _send_model_request();

	// Response processing (called when response arrives)
	void _process_model_response(const Dictionary &p_response);

	// Internal processing methods
	bool _validate_response(const Dictionary &p_response, String &r_error);
	bool _is_final_response(const Dictionary &p_response);
	Array _execute_actions(const Array &p_actions);
	Dictionary _create_tool_result(const String &p_action_type, const Dictionary &p_action_args, const Dictionary &p_exec_result);
	Dictionary _create_validation_error_result(const String &p_error_message, const String &p_raw_response);
	String _generate_action_id();

	// Guardrail handlers
	void _handle_cancellation();
	void _handle_max_turns_exceeded();
	void _handle_max_actions_exceeded();
	void _handle_max_repairs_exceeded(const String &p_validation_error);

	// Signal emissions
	void _emit_progress_update(const String &p_status, int p_turn);
	void _emit_tool_result(const Dictionary &p_tool_result);
	void _emit_run_complete(bool p_success, const String &p_final_message);

	// Helper to format tool result for display
	String _format_tool_result_for_display(const Dictionary &p_tool_result);
};

#endif // AGENTIC_ORCHESTRATOR_H
