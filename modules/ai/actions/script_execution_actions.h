// modules/ai/actions/script_execution_actions.h
// Script-execution action for the AI module: run agent-authored GDScript
// in-process against the live editor API. Design: docs/design/script_execution_plan.md §4.

#ifndef AI_SCRIPT_EXECUTION_ACTIONS_H
#define AI_SCRIPT_EXECUTION_ACTIONS_H

#include "core/variant/dictionary.h"

namespace AIScriptExecActions {

// Compiles the given GDScript source in memory (never written to disk) and
// calls its `func run() -> Variant` entry point on the editor main thread,
// against live editor state.
// Required args: script (String — full GDScript source defining run())
// Returns: Dictionary with status="success"|"error". Success result carries
// return_value, return_type, prints, execution_time_ms, and errors (when any
// engine errors were raised during execution). Parse/compile failures carry
// structured errors with line numbers; runtime aborts carry the captured
// script errors.
Dictionary exec_run_editor_script(const Dictionary &args);

} // namespace AIScriptExecActions

#endif // AI_SCRIPT_EXECUTION_ACTIONS_H
