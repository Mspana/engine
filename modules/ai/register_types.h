#ifndef AI_REGISTER_TYPES_H
#define AI_REGISTER_TYPES_H

#include "modules/register_module_types.h" // Required for ModuleInitializationLevel

// Declaration for the module initialization function.
void initialize_ai_module(ModuleInitializationLevel p_level);

// Declaration for the module uninitialization function.
void uninitialize_ai_module(ModuleInitializationLevel p_level);

#endif // AI_REGISTER_TYPES_H 