// modules/ai/actions/signal_actions.h
// Signal-related action implementations for the AI module

#ifndef AI_SIGNAL_ACTIONS_H
#define AI_SIGNAL_ACTIONS_H

#include "core/variant/dictionary.h"

namespace AISignalActions {

// Connects a signal from an emitter node to a target method.
// Required args: emitter_path (String), signal_name (String), target_path (String), method_name (String)
// Optional args: binds (Array), flags (int)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_connect_signal(const Dictionary &args);

// Disconnects a signal from an emitter node to a target method.
// Required args: emitter_path (String), signal_name (String), target_path (String), method_name (String)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_disconnect_signal(const Dictionary &args);

} // namespace AISignalActions

#endif // AI_SIGNAL_ACTIONS_H

