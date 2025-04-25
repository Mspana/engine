#include "register_types.h"

#include "ai.h" // Include the header for the class we are registering

#include "core/config/engine.h" // Required for Engine singleton
#include "core/object/class_db.h" // Required for ClassDB

// Module initialization function.
void initialize_ai_module(ModuleInitializationLevel p_level) {
	// Initialize the singleton at the SCENE level.
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	// Register the AI class itself.
	ClassDB::register_class<AI>();

	// Create the singleton instance using the class's own method.
	AI::initialize_singleton();

	// Register the singleton with the Engine's singleton map.
	// This makes it globally accessible, e.g., `AI` in GDScript.
	Engine::get_singleton()->add_singleton(Engine::Singleton("AI", AI::get_singleton()));
}

// Module uninitialization function.
void uninitialize_ai_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	// Remove the singleton from the Engine.
	Engine::get_singleton()->remove_singleton("AI");

	// Clean up the singleton instance using the class's own method.
	AI::finalize_singleton();
} 