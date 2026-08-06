#include "register_types.h"

#include "ai.h" // Include the header for the class we are registering
#include "ai_provider.h" // Include provider classes
#include "harness/codex_harness_driver.h"
#include "harness/responses_translator.h"
#include "retrieval.h" // Include retrieval class

#include "core/config/engine.h" // Required for Engine singleton
#include "core/object/class_db.h" // Required for ClassDB
#include "core/os/os.h"

#ifdef TOOLS_ENABLED
#include "editor/ai_chat_store.h"
#include "editor/ai_image_widgets.h"
#include "editor/ai_status_indicator.h"
#include "editor/ai_remote_dialog.h"
#include "editor/gif_import_handler.h"
#include "editor/plugins/editor_plugin.h"
#include "remote/ai_remote_server.h"
#include "session/ai_chat_session.h"
#endif

// Keeps the smoke-test driver alive for the editor session (see below).
static Ref<CodexHarnessDriver> _harness_smoke_driver;

#ifdef TOOLS_ENABLED
// Session seam + remote access. Both are editor-lifetime singletons; the remote
// server stays stopped until the user explicitly enables it.
static AIChatSession *_ai_chat_session = nullptr;
static AIRemoteServer *_ai_remote_server = nullptr;
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
		ClassDB::register_class<ClarifaiProvider>();
		ClassDB::register_class<MoonshotProvider>();

		// Register retrieval class
		ClassDB::register_class<RetrievalIndex>();

		// Create the singleton instance using the class's own method.
		AI::initialize_singleton();

		// Register the singleton with the Engine's singleton map.
		// This makes it globally accessible, e.g., `AI` in GDScript.
		Engine::get_singleton()->add_singleton(Engine::Singleton("AI", AI::get_singleton()));

		ClassDB::register_class<CodexHarnessDriver>();

		// Native Responses->Chat translator for the codex harness (replaces the
		// LiteLLM sidecar). Spike-gated by env var until the harness driver
		// owns its lifecycle: set ARISTOTLE_TRANSLATOR_PORT=4123 to enable.
		String translator_port = OS::get_singleton()->get_environment("ARISTOTLE_TRANSLATOR_PORT");
		if (!translator_port.is_empty()) {
			AIResponsesTranslator::get_singleton()->start(translator_port.to_int());
		}

		// Headless smoke path for the harness driver: set ARISTOTLE_HARNESS_SMOKE
		// to a prompt and the driver runs one turn at startup, printing events.
		String smoke_prompt = OS::get_singleton()->get_environment("ARISTOTLE_HARNESS_SMOKE");
		if (!smoke_prompt.is_empty()) {
			_harness_smoke_driver.instantiate();
			if (_harness_smoke_driver->start_session()) {
				_harness_smoke_driver->send_user_message(smoke_prompt, 1);
			} else {
				_harness_smoke_driver.unref();
			}
		}
	}

#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		// Register editor plugin classes
		GDREGISTER_CLASS(AIChatStore);
		GDREGISTER_CLASS(AIChatSession);
		GDREGISTER_CLASS(AIRemoteServer);
		GDREGISTER_CLASS(AIRemoteDialog);
		_ai_chat_session = memnew(AIChatSession);
		_ai_remote_server = memnew(AIRemoteServer);

		// Remote access is off unless the user enables it in the panel. This
		// env var exists for headless testing and starts the SAME code path;
		// "lan" still refuses non-private peers.
		const String remote_mode = OS::get_singleton()->get_environment("ARISTOTLE_REMOTE");
		if (!remote_mode.is_empty()) {
			if (remote_mode == "relay") {
				// Outbound transport: dials the relay, opens no local port.
				const String url = OS::get_singleton()->get_environment("ARISTOTLE_RELAY_URL");
				if (!url.is_empty()) {
					_ai_remote_server->start_relay(url);
				} else {
					ERR_PRINT("ARISTOTLE_REMOTE=relay needs ARISTOTLE_RELAY_URL.");
				}
			} else {
				const String port_env = OS::get_singleton()->get_environment("ARISTOTLE_REMOTE_PORT");
				const int remote_port = port_env.is_empty() ? (int)AIRemoteServer::DEFAULT_PORT : port_env.to_int();
				_ai_remote_server->start(remote_mode == "lan" ? AIRemoteServer::BIND_LAN : AIRemoteServer::BIND_LOOPBACK,
						remote_port);
			}

			// Opens a normal pairing window at startup and prints the code, so
			// a headless run can be paired without the dialog. Same code path,
			// same random single-use secret, same two-minute expiry — reading
			// it already requires local access to this process's output.
			if (OS::get_singleton()->get_environment("ARISTOTLE_REMOTE_PAIR") == "1") {
				const String code = _ai_remote_server->begin_pairing();
				if (!code.is_empty()) {
					print_line("ARISTOTLE_PAIRING_CODE=" + code);
				}
			}
		}
		GDREGISTER_CLASS(AIImageStack);
		GDREGISTER_CLASS(AIImageViewer);
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
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		if (_ai_remote_server) {
			_ai_remote_server->stop();
			memdelete(_ai_remote_server);
			_ai_remote_server = nullptr;
		}
		if (_ai_chat_session) {
			memdelete(_ai_chat_session);
			_ai_chat_session = nullptr;
		}
	}
#endif
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	// Remove the singleton from the Engine.
	Engine::get_singleton()->remove_singleton("AI");

	if (_harness_smoke_driver.is_valid()) {
		_harness_smoke_driver->shutdown();
		_harness_smoke_driver.unref();
	}
	AIResponsesTranslator::get_singleton()->stop();

	// Clean up the singleton instance using the class's own method.
	AI::finalize_singleton();
}
