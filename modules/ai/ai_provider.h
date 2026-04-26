#ifndef AI_PROVIDER_H
#define AI_PROVIDER_H

#include "core/object/ref_counted.h"
#include "core/string/ustring.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "core/io/http_client.h"
#include "core/object/worker_thread_pool.h"

// Base class for AI API providers
class AIProvider : public RefCounted {
	GDCLASS(AIProvider, RefCounted);

protected:
	String api_key;
	String model;
	float temperature;
	int max_tokens;
	String base_url;

	// End-to-end HTTP round-trip latency of the most recent request, in ms.
	// Set by subclasses inside their `_perform_request*` functions after the
	// response body has been fully received. Zero means "no sample yet."
	int64_t _last_request_latency_ms = 0;

	static void _bind_methods();

public:
	int64_t get_last_request_latency_ms() const { return _last_request_latency_ms; }
	// Signal emitted when request completes
	// Parameters: success (bool), response_json (String), error_message (String)
	
	// Getters and setters
	void set_api_key(const String &p_api_key);
	String get_api_key() const;

	void set_model(const String &p_model);
	String get_model() const;

	void set_temperature(float p_temperature);
	float get_temperature() const;

	void set_max_tokens(int p_max_tokens);
	int get_max_tokens() const;

	void set_base_url(const String &p_base_url);
	String get_base_url() const;

	// Virtual methods for provider-specific implementation
	virtual String get_default_base_url() const;
	virtual String get_default_model() const;
	virtual Dictionary build_request_body(const String &user_prompt, const String &context_block = "") const;
	virtual String parse_response(const Dictionary &response_data) const;
	virtual PackedStringArray get_request_headers() const;
	virtual String get_request_url() const;

	// System prompt explaining the action format (legacy JSON format)
	static String get_system_prompt();

	// System prompt for native tool-calling (no JSON schema, tools defined in API request)
	static String get_system_prompt_native_tools();

	// Build OpenAI-compatible tools array for native tool-calling
	static Array build_tools_array();

	// Model catalog entry for the dropdown
	struct ModelEntry {
		String model_id;
		String display_name;
		String provider; // "xai", "openai", "anthropic", "gemini"
	};
	static Vector<ModelEntry> get_available_models();

	// Vision capability check — returns true if the named model accepts image input
	static bool model_supports_vision(const String &p_model);
	bool supports_vision() const { return model_supports_vision(model); }

	// Context window size in tokens for a given model name.
	// Returns 0 for unknown models.
	static int get_context_window_tokens(const String &p_model);
	int get_context_window_tokens() const { return get_context_window_tokens(model); }
	
	// Main method to send request - implemented by subclasses (single message, legacy)
	virtual void send_request(const String &user_prompt, const String &context_block = "");
	
	// New method that accepts full conversation history
	virtual void send_request_with_messages(const Array &p_messages, const String &context_block = "");
	
	// New method to build request body from message history
	virtual Dictionary build_request_body_with_messages(const Array &p_messages, const String &context_block = "") const;
	
	// Provider name for logging/dashboard
	virtual String get_provider_name() const { return "unknown"; }

	// Whether the host accepts image_url parts in `tool` role messages. OpenAI does;
	// DeepInfra does not (its schema only allows text parts in tool content; HTTP 422).
	virtual bool supports_image_in_tool_content() const { return true; }

	// Helper to load API key from environment or .env file
	static String load_api_key_from_env(const String &env_var_name);
	static String load_from_env_file(const String &key_name, const String &env_file_path = ".env");

	AIProvider();
	virtual ~AIProvider();
};

// OpenAI Provider (gpt-4o-mini, gpt-4, etc.)
class OpenAIProvider : public AIProvider {
	GDCLASS(OpenAIProvider, AIProvider);

protected:
	void _perform_request(const String &user_prompt, const String &context_block);
	void _perform_request_with_messages(const Array &p_messages, const String &context_block);
	
	static void _bind_methods();

public:
	virtual String get_default_base_url() const override;
	virtual String get_default_model() const override;
	virtual Dictionary build_request_body(const String &user_prompt, const String &context_block = "") const override;
	virtual Dictionary build_request_body_with_messages(const Array &p_messages, const String &context_block = "") const override;
	virtual String parse_response(const Dictionary &response_data) const override;
	virtual PackedStringArray get_request_headers() const override;
	virtual String get_request_url() const override;
	virtual void send_request(const String &user_prompt, const String &context_block = "") override;
	virtual void send_request_with_messages(const Array &p_messages, const String &context_block = "") override;
	virtual String get_provider_name() const override { return "openai"; }

	// Transport hooks — subclasses (e.g. DeepInfra) override these to redirect the HTTP call
	// while reusing the OpenAI request/response format.
	virtual String get_request_host() const;
	virtual String get_request_path() const;

	OpenAIProvider();
	~OpenAIProvider();
};

// Google Gemini Provider
class GeminiProvider : public AIProvider {
	GDCLASS(GeminiProvider, AIProvider);

protected:
	void _perform_request(const String &user_prompt, const String &context_block);
	void _perform_request_with_messages(const Array &p_messages, const String &context_block);
	
	static void _bind_methods();

public:
	virtual String get_default_base_url() const override;
	virtual String get_default_model() const override;
	virtual Dictionary build_request_body(const String &user_prompt, const String &context_block = "") const override;
	virtual Dictionary build_request_body_with_messages(const Array &p_messages, const String &context_block = "") const override;
	virtual String parse_response(const Dictionary &response_data) const override;
	virtual PackedStringArray get_request_headers() const override;
	virtual String get_request_url() const override;
	virtual void send_request(const String &user_prompt, const String &context_block = "") override;
	virtual void send_request_with_messages(const Array &p_messages, const String &context_block = "") override;
	virtual String get_provider_name() const override { return "gemini"; }

	GeminiProvider();
	~GeminiProvider();
};

// x.ai Provider (Grok)
class XAIProvider : public AIProvider {
	GDCLASS(XAIProvider, AIProvider);

protected:
	void _perform_request(const String &user_prompt, const String &context_block);
	void _perform_request_with_messages(const Array &p_messages, const String &context_block);
	
	static void _bind_methods();

public:
	virtual String get_default_base_url() const override;
	virtual String get_default_model() const override;
	virtual Dictionary build_request_body(const String &user_prompt, const String &context_block = "") const override;
	virtual Dictionary build_request_body_with_messages(const Array &p_messages, const String &context_block = "") const override;
	virtual String parse_response(const Dictionary &response_data) const override;
	virtual PackedStringArray get_request_headers() const override;
	virtual String get_request_url() const override;
	virtual void send_request(const String &user_prompt, const String &context_block = "") override;
	virtual void send_request_with_messages(const Array &p_messages, const String &context_block = "") override;
	virtual String get_provider_name() const override { return "xai"; }

	XAIProvider();
	~XAIProvider();
};

// Anthropic Provider (Claude)
class AnthropicProvider : public AIProvider {
	GDCLASS(AnthropicProvider, AIProvider);

protected:
	void _perform_request(const String &user_prompt, const String &context_block);
	void _perform_request_with_messages(const Array &p_messages, const String &context_block);

	static void _bind_methods();

public:
	virtual String get_default_base_url() const override;
	virtual String get_default_model() const override;
	virtual Dictionary build_request_body(const String &user_prompt, const String &context_block = "") const override;
	virtual Dictionary build_request_body_with_messages(const Array &p_messages, const String &context_block = "") const override;
	virtual String parse_response(const Dictionary &response_data) const override;
	virtual PackedStringArray get_request_headers() const override;
	virtual String get_request_url() const override;
	virtual void send_request(const String &user_prompt, const String &context_block = "") override;
	virtual void send_request_with_messages(const Array &p_messages, const String &context_block = "") override;
	virtual String get_provider_name() const override { return "anthropic"; }

	AnthropicProvider();
	~AnthropicProvider();
};

// DeepInfra Provider — OpenAI-compatible host for open-source models (Kimi, Qwen, DeepSeek, etc.)
class DeepInfraProvider : public OpenAIProvider {
	GDCLASS(DeepInfraProvider, OpenAIProvider);

protected:
	static void _bind_methods();

public:
	virtual String get_default_base_url() const override;
	virtual String get_default_model() const override;
	virtual String get_request_host() const override;
	virtual String get_request_path() const override;
	virtual String get_provider_name() const override { return "deepinfra"; }
	virtual bool supports_image_in_tool_content() const override { return false; }

	DeepInfraProvider();
	~DeepInfraProvider();
};

// Parasail Provider — OpenAI-compatible host for open-source models. Used for
// Kimi K2.6 because DeepInfra hasn't enabled multimodal dispatch for that model.
class ParasailProvider : public OpenAIProvider {
	GDCLASS(ParasailProvider, OpenAIProvider);

protected:
	static void _bind_methods();

public:
	virtual String get_default_base_url() const override;
	virtual String get_default_model() const override;
	virtual String get_request_host() const override;
	virtual String get_request_path() const override;
	virtual String get_provider_name() const override { return "parasail"; }

	ParasailProvider();
	~ParasailProvider();
};

// Dummy Provider (for testing/simulation)
class DummyProvider : public AIProvider {
	GDCLASS(DummyProvider, AIProvider);

protected:
	static void _bind_methods();

public:
	virtual String get_default_base_url() const override;
	virtual String get_default_model() const override;
	virtual Dictionary build_request_body(const String &user_prompt, const String &context_block = "") const override;
	virtual Dictionary build_request_body_with_messages(const Array &p_messages, const String &context_block = "") const override;
	virtual String parse_response(const Dictionary &response_data) const override;
	virtual PackedStringArray get_request_headers() const override;
	virtual String get_request_url() const override;
	virtual void send_request(const String &user_prompt, const String &context_block = "") override;
	virtual void send_request_with_messages(const Array &p_messages, const String &context_block = "") override;
	virtual String get_provider_name() const override { return "dummy"; }

	// Returns simulated response directly (doesn't need HTTP)
	String get_dummy_response(const String &user_prompt) const;

	DummyProvider();
};

#endif // AI_PROVIDER_H

