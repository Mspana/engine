#include "register_types.h"

#include "ai.h" // Include the header for the class we are registering
#include "ai_provider.h" // Include provider classes
#include "retrieval.h" // Include retrieval class

#include "core/config/engine.h" // Required for Engine singleton
#include "core/object/class_db.h" // Required for ClassDB

#ifdef TOOLS_ENABLED
#include "editor/ai_chat_store.h"
#include "editor/ai_status_indicator.h"
#include "editor/gif_import_handler.h"
#include "editor/plugins/editor_plugin.h"
#endif

// Module initialization function.
void initialize_ai_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		// Register the AI class itself.
		ClassDB::register_class<AI>();
		
		// Register AI provider classes
		ClassDB::register_class<AIProvider>();
		ClassDB::register_class<DummyProvider>();
		ClassDB::register_class<OpenAIProvider>();
		ClassDB::register_class<GeminiProvider>();
		ClassDB::register_class<XAIProvider>();
		ClassDB::register_class<AnthropicProvider>();
		ClassDB::register_class<DeepInfraProvider>();
		ClassDB::register_class<ParasailProvider>();
		
		// Register retrieval class
		ClassDB::register_class<RetrievalIndex>();

		// Create the singleton instance using the class's own method.
		AI::initialize_singleton();

		// Register the singleton with the Engine's singleton map.
		// This makes it globally accessible, e.g., `AI` in GDScript.
		Engine::get_singleton()->add_singleton(Engine::Singleton("AI", AI::get_singleton()));
	}

#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		// Register editor plugin classes
		GDREGISTER_CLASS(AIChatStore);
		GDREGISTER_CLASS(ThinkingCollapsibleEntry);
		GDREGISTER_CLASS(ToolCollapsibleEntry);
GDREGISTER_CLASS(AIStatusIndicator);
		GDREGISTER_CLASS(AIStatusPanel);
		GDREGISTER_CLASS(AIStatusIndicatorPlugin);
		EditorPlugins::add_by_type<AIStatusIndicatorPlugin>();
		GDREGISTER_CLASS(GIFImportHandler);
		EditorPlugins::add_by_type<GIFImportHandler>();
	}
#endif
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
