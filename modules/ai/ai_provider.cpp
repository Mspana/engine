// modules/ai/ai_provider.cpp
#include "ai_provider.h"

#include "core/io/json.h"
#include "core/variant/variant.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/crypto/crypto.h"

// ============================================================================
// AIProvider - Base Class Implementation
// ============================================================================

AIProvider::AIProvider() {
	temperature = 0.7f;
	max_tokens = 8000;  // Increased to allow full script content in responses
	model = get_default_model();
	base_url = get_default_base_url();
}

AIProvider::~AIProvider() {
}

void AIProvider::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_api_key", "api_key"), &AIProvider::set_api_key);
	ClassDB::bind_method(D_METHOD("get_api_key"), &AIProvider::get_api_key);

	ClassDB::bind_method(D_METHOD("set_model", "model"), &AIProvider::set_model);
	ClassDB::bind_method(D_METHOD("get_model"), &AIProvider::get_model);

	ClassDB::bind_method(D_METHOD("set_temperature", "temperature"), &AIProvider::set_temperature);
	ClassDB::bind_method(D_METHOD("get_temperature"), &AIProvider::get_temperature);

	ClassDB::bind_method(D_METHOD("set_max_tokens", "max_tokens"), &AIProvider::set_max_tokens);
	ClassDB::bind_method(D_METHOD("get_max_tokens"), &AIProvider::get_max_tokens);

	ClassDB::bind_method(D_METHOD("set_base_url", "base_url"), &AIProvider::set_base_url);
	ClassDB::bind_method(D_METHOD("get_base_url"), &AIProvider::get_base_url);
	
	ClassDB::bind_method(D_METHOD("send_request", "user_prompt"), &AIProvider::send_request);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "api_key"), "set_api_key", "get_api_key");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "model"), "set_model", "get_model");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "temperature"), "set_temperature", "get_temperature");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_tokens"), "set_max_tokens", "get_max_tokens");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "base_url"), "set_base_url", "get_base_url");
	
	// Signal: request_completed(success: bool, response_json: String, error_message: String)
	ADD_SIGNAL(MethodInfo("request_completed", PropertyInfo(Variant::BOOL, "success"), PropertyInfo(Variant::STRING, "response_json"), PropertyInfo(Variant::STRING, "error_message")));
}

// Getters and Setters
void AIProvider::set_api_key(const String &p_api_key) {
	api_key = p_api_key;
}

String AIProvider::get_api_key() const {
	return api_key;
}

void AIProvider::set_model(const String &p_model) {
	model = p_model;
}

String AIProvider::get_model() const {
	return model;
}

void AIProvider::set_temperature(float p_temperature) {
	temperature = p_temperature;
}

float AIProvider::get_temperature() const {
	return temperature;
}

void AIProvider::set_max_tokens(int p_max_tokens) {
	max_tokens = p_max_tokens;
}

int AIProvider::get_max_tokens() const {
	return max_tokens;
}

void AIProvider::set_base_url(const String &p_base_url) {
	base_url = p_base_url;
}

String AIProvider::get_base_url() const {
	return base_url;
}

// Virtual method defaults
String AIProvider::get_default_base_url() const {
	return "";
}

String AIProvider::get_default_model() const {
	return "";
}

Dictionary AIProvider::build_request_body(const String &user_prompt, const String &context_block) const {
	return Dictionary();
}

String AIProvider::parse_response(const Dictionary &response_data) const {
	return "";
}

PackedStringArray AIProvider::get_request_headers() const {
	PackedStringArray headers;
	return headers;
}

String AIProvider::get_request_url() const {
	return "";
}

void AIProvider::send_request(const String &user_prompt, const String &context_block) {
	ERR_PRINT("AIProvider::send_request() - Base class method called. Override in subclass.");
	emit_signal("request_completed", false, "", "Provider does not implement send_request()");
}

String AIProvider::load_api_key_from_env(const String &env_var_name) {
	// First try system environment variable
	String env_value = OS::get_singleton()->get_environment(env_var_name);
	if (!env_value.is_empty()) {
		return env_value;
	}
	
	// If not found, try .env file in executable directory
	String key_from_file = load_from_env_file(env_var_name);
	if (!key_from_file.is_empty()) {
		return key_from_file;
	}
	
	return "";
}

String AIProvider::load_from_env_file(const String &key_name, const String &env_file_path) {
	// Read .env file from executable directory (where the engine/editor is)
	// API keys are engine-level, not project-specific
	String exe_dir = OS::get_singleton()->get_executable_path().get_base_dir();
	
	// Try the executable directory first (bin/.env)
	String full_path = exe_dir.path_join(env_file_path);
	Ref<FileAccess> file = FileAccess::open(full_path, FileAccess::READ);
	
	// If not found in bin/, try the parent directory (engine root)
	if (file.is_null()) {
		String parent_dir = exe_dir.get_base_dir(); // Go up one level from bin to engine root
		full_path = parent_dir.path_join(env_file_path);
		file = FileAccess::open(full_path, FileAccess::READ);
	}
	
	if (file.is_null()) {
		// .env file doesn't exist, which is fine
		return "";
	}
	
	// Parse the .env file line by line
	while (!file->eof_reached()) {
		String line = file->get_line().strip_edges();
		
		// Skip empty lines and comments
		if (line.is_empty() || line.begins_with("#")) {
			continue;
		}
		
		// Parse KEY=VALUE format
		int equals_pos = line.find("=");
		if (equals_pos != -1) {
			String file_key = line.substr(0, equals_pos).strip_edges();
			String file_value = line.substr(equals_pos + 1).strip_edges();
			
			// Remove quotes if present
			if ((file_value.begins_with("\"") && file_value.ends_with("\"")) ||
			    (file_value.begins_with("'") && file_value.ends_with("'"))) {
				file_value = file_value.substr(1, file_value.length() - 2);
			}
			
			if (file_key == key_name) {
				return file_value;
			}
		}
	}
	
	return "";
}

String AIProvider::get_system_prompt() {
	return "You are a Godot 4 AI helper.\n"
	       "You are helping kids ages 12–18 build games.\n"
	       "You MUST always output a JSON array of actions and nothing else.\n"
	       "Never use Godot 3 APIs.\n"
	       "Prefer modifying existing scripts rather than generating new ones.\n"
	       "No explanations, no comments, no prose.\n"
	       "\n"
	       "Format: [{\"action\": \"...\", \"args\": {...}}, ...]\n"
	       "\n"
	       "Allowed actions:\n"
	       "- create_node: Create a new node in the scene tree\n"
	       "  Args: {\"node_name\": string, \"node_type\": string, \"parent_path\": string (optional)}\n"
	       "- delete_node: Delete a node from the scene tree\n"
	       "  Args: {\"node_path\": string}\n"
	       "- duplicate_node: Duplicate a node in the scene tree\n"
	       "  Args: {\"node_path\": string, \"new_name\": string (optional, defaults to \"<old_name>_copy\")}\n"
	       "- set_property: Set a property on an existing node\n"
	       "  Args: {\"node_path\": string, \"property_name\": string, \"value\": any}\n"
	       "- rename_node: Rename a node (prefer this over set_property for name changes)\n"
	       "  Args: {\"node_path\": string, \"new_name\": string}\n"
	       "- reparent_node: Move a node to a new parent\n"
	       "  Args: {\"node_path\": string, \"new_parent_path\": string, \"index\": int (optional)}\n"
	       "- create_script: Create a new script file (GDScript only)\n"
	       "  Args: {\"file_path\": string (e.g. \"res://scripts/Enemy.gd\"), \"language\": \"GDScript\", \"content\": string}\n"
	       "- update_script: Update an existing script file with new content\n"
	       "  Args: {\"file_path\": string, \"patch\": string (full file content)}\n"
	       "- attach_script: Attach a script to a node\n"
	       "  Args: {\"node_path\": string, \"script_path\": string}\n"
	       "- detach_script: Detach a script from a node\n"
	       "  Args: {\"node_path\": string}\n"
	       "- rename_script: Rename/move a script file\n"
	       "  Args: {\"old_path\": string, \"new_path\": string}\n"
	       "- delete_script: Delete a script file\n"
	       "  Args: {\"file_path\": string, \"detach_from_nodes\": bool (optional, default false)}\n"
	       "- connect_signal: Connect a signal from an emitter node to a target method\n"
	       "  Args: {\"emitter_path\": string, \"signal_name\": string (e.g. \"pressed\"), \"target_path\": string, \"method_name\": string (e.g. \"_on_button_pressed\"), \"binds\": array (optional), \"flags\": int (optional)}\n"
	       "- disconnect_signal: Disconnect a signal from an emitter node to a target method\n"
	       "  Args: {\"emitter_path\": string, \"signal_name\": string, \"target_path\": string, \"method_name\": string}\n"
	       "- create_scene: Create a new scene file\n"
	       "  Args: {\"scene_path\": string (e.g. \"res://scenes/Main.tscn\"), \"root_type\": string (optional, default \"Node\"), \"root_name\": string (optional, default \"Main\")}\n"
	       "- open_scene: Open a scene file in the editor\n"
	       "  Args: {\"scene_path\": string (e.g. \"res://scenes/Main.tscn\")}\n"
	       "- save_scene: Save the currently edited scene\n"
	       "  Args: {}\n"
	       "- close_scene: Close the current scene tab\n"
	       "  Args: {\"save_if_modified\": bool (optional, default true)}\n"
	       "- set_main_scene: Set the project's main scene in ProjectSettings\n"
	       "  Args: {\"scene_path\": string (e.g. \"res://scenes/Main.tscn\")}\n"
	       "- set_project_setting: Set a project setting value\n"
	       "  Args: {\"key\": string (e.g. \"display/window/size/viewport_width\"), \"value\": any}\n"
	       "- list_nodes: List nodes in the current scene tree\n"
	       "  Args: {\"root_path\": string (optional)}\n"
	       "- get_node_info: Get detailed information about a single node\n"
	       "  Args: {\"node_path\": string}\n"
	       "- find_nodes_by_type: Find all nodes of a specific type in the scene tree\n"
	       "  Args: {\"type_name\": string (e.g. \"CharacterBody2D\")}\n"
	       "- list_files: List files in a directory\n"
	       "  Args: {\"directory\": string (e.g. \"res://scripts\"), \"glob\": string (optional, e.g. \"*.gd\")}";
}

// ============================================================================
// OpenAIProvider Implementation
// ============================================================================

OpenAIProvider::OpenAIProvider() : AIProvider() {
	model = get_default_model();
	base_url = get_default_base_url();
	
	// Try to load API key from environment
	String env_key = load_api_key_from_env("OPENAI_API_KEY");
	if (!env_key.is_empty()) {
		api_key = env_key;
	}
}

OpenAIProvider::~OpenAIProvider() {
}

void OpenAIProvider::_bind_methods() {
	// No additional methods to bind for now
}

String OpenAIProvider::get_default_base_url() const {
	return "https://api.openai.com";
}

String OpenAIProvider::get_default_model() const {
	return "gpt-4o-mini";
}

Dictionary OpenAIProvider::build_request_body(const String &user_prompt, const String &context_block) const {
	Dictionary body;
	body["model"] = model;
	body["temperature"] = temperature;
	body["max_tokens"] = max_tokens;

	// Build messages array	
	Array messages;
	
	Dictionary system_msg;
	system_msg["role"] = "system";
	String system_content = get_system_prompt();
	if (!context_block.is_empty()) {
		system_content += context_block;
	}
	system_msg["content"] = system_content;
	messages.push_back(system_msg);

	Dictionary user_msg;
	user_msg["role"] = "user";
	user_msg["content"] = user_prompt;
	messages.push_back(user_msg);

	body["messages"] = messages;

	return body;
}

String OpenAIProvider::parse_response(const Dictionary &response_data) const {
	// OpenAI response format: {"choices": [{"message": {"content": "..."}}]}
	if (!response_data.has("choices")) {
		ERR_PRINT("OpenAI response missing 'choices' field");
		return "";
	}

	Array choices = response_data["choices"];
	if (choices.is_empty()) {
		ERR_PRINT("OpenAI response 'choices' array is empty");
		return "";
	}

	Dictionary first_choice = choices[0];
	if (!first_choice.has("message")) {
		ERR_PRINT("OpenAI response choice missing 'message' field");
		return "";
	}

	Dictionary message = first_choice["message"];
	if (!message.has("content")) {
		ERR_PRINT("OpenAI response message missing 'content' field");
		return "";
	}

	return message["content"];
}

PackedStringArray OpenAIProvider::get_request_headers() const {
	PackedStringArray headers;
	headers.push_back("Content-Type: application/json");
	headers.push_back("Authorization: Bearer " + api_key);
	return headers;
}

String OpenAIProvider::get_request_url() const {
	return base_url + "/v1/chat/completions";
}

void OpenAIProvider::send_request(const String &user_prompt, const String &context_block) {
	if (api_key.is_empty()) {
		ERR_PRINT("OpenAIProvider::send_request() - API key is not set");
		call_deferred("emit_signal", "request_completed", false, "", "API key is not set");
		return;
	}
	
	print_line(vformat("OpenAIProvider: Sending request to %s", get_request_url()));
	
	// Submit task to worker thread pool
	WorkerThreadPool::get_singleton()->add_task(
		callable_mp(this, &OpenAIProvider::_perform_request).bind(user_prompt, context_block)
	);
}

void OpenAIProvider::_perform_request(const String &user_prompt, const String &context_block) {
	HTTPClient *http_client = HTTPClient::create();
	
	// Parse URL to extract host and path
	String url = get_request_url();
	String host = "api.openai.com";
	int port = 443;
	
	// Connect to host
	Ref<TLSOptions> tls_options = TLSOptions::client();
	Error err = http_client->connect_to_host(host, port, tls_options);
	if (err != OK) {
		ERR_PRINT(vformat("OpenAIProvider: Failed to connect to host: %d", err));
		call_deferred("emit_signal", "request_completed", false, "", vformat("Failed to connect: %d", err));
		memdelete(http_client);
		return;
	}
	
	// Wait for connection
	while (http_client->get_status() == HTTPClient::STATUS_CONNECTING ||
	       http_client->get_status() == HTTPClient::STATUS_RESOLVING) {
		http_client->poll();
		OS::get_singleton()->delay_usec(10000); // 10ms
	}
	
	if (http_client->get_status() != HTTPClient::STATUS_CONNECTED) {
		ERR_PRINT(vformat("OpenAIProvider: Connection failed, status: %d", http_client->get_status()));
		call_deferred("emit_signal", "request_completed", false, "", "Connection failed");
		memdelete(http_client);
		return;
	}
	
	// Build request
	Dictionary request_body = build_request_body(user_prompt, context_block);
	String json_body = JSON::stringify(request_body);
	PackedStringArray headers_array = get_request_headers();
	
	Vector<String> headers_vector;
	for (int i = 0; i < headers_array.size(); i++) {
		headers_vector.push_back(headers_array[i]);
	}
	
	// Send request
	CharString body_data = json_body.utf8();
	err = http_client->request(HTTPClient::METHOD_POST, "/v1/chat/completions", headers_vector, (const uint8_t *)body_data.get_data(), body_data.length());
	if (err != OK) {
		ERR_PRINT(vformat("OpenAIProvider: Failed to send request: %d", err));
		call_deferred("emit_signal", "request_completed", false, "", vformat("Failed to send request: %d", err));
		memdelete(http_client);
		return;
	}
	
	// Wait for response
	while (http_client->get_status() == HTTPClient::STATUS_REQUESTING) {
		http_client->poll();
		OS::get_singleton()->delay_usec(10000); // 10ms
	}
	
	if (http_client->get_status() != HTTPClient::STATUS_BODY &&
	    http_client->get_status() != HTTPClient::STATUS_CONNECTED) {
		ERR_PRINT(vformat("OpenAIProvider: Request failed, status: %d", http_client->get_status()));
		call_deferred("emit_signal", "request_completed", false, "", "Request failed");
		memdelete(http_client);
		return;
	}
	
	// Check response code
	int response_code = http_client->get_response_code();
	if (response_code != 200) {
		ERR_PRINT(vformat("OpenAIProvider: HTTP error code: %d", response_code));
		call_deferred("emit_signal", "request_completed", false, "", vformat("HTTP error: %d", response_code));
		memdelete(http_client);
		return;
	}
	
	// Read response body
	PackedByteArray response_body;
	while (http_client->get_status() == HTTPClient::STATUS_BODY) {
		http_client->poll();
		PackedByteArray chunk = http_client->read_response_body_chunk();
		if (chunk.size() > 0) {
			response_body.append_array(chunk);
		} else {
			OS::get_singleton()->delay_usec(10000); // 10ms
		}
	}
	
	String response_str = String::utf8((const char *)response_body.ptr(), response_body.size());
	
	// Parse JSON response
	JSON json_parser;
	err = json_parser.parse(response_str);
	if (err != Error::OK) {
		ERR_PRINT(vformat("OpenAIProvider: Failed to parse response JSON: %s", json_parser.get_error_message()));
		call_deferred("emit_signal", "request_completed", false, "", "Failed to parse response JSON");
		memdelete(http_client);
		return;
	}
	
	Dictionary response_data = json_parser.get_data();
	String ai_response = parse_response(response_data);
	
	if (ai_response.is_empty()) {
		ERR_PRINT("OpenAIProvider: Empty response from AI");
		call_deferred("emit_signal", "request_completed", false, "", "Empty response");
		memdelete(http_client);
		return;
	}
	
	print_line(vformat("OpenAIProvider: Received response: %s", ai_response));
	call_deferred("emit_signal", "request_completed", true, ai_response, "");
	
	// Clean up
	memdelete(http_client);
}

// ============================================================================
// GeminiProvider Implementation
// ============================================================================

GeminiProvider::GeminiProvider() : AIProvider() {
	model = get_default_model();
	base_url = get_default_base_url();
	
	// Try to load API key from environment
	String env_key = load_api_key_from_env("GEMINI_API_KEY");
	if (!env_key.is_empty()) {
		api_key = env_key;
	}
}

GeminiProvider::~GeminiProvider() {
}

void GeminiProvider::_bind_methods() {
	// No additional methods to bind for now
}

String GeminiProvider::get_default_base_url() const {
	return "https://generativelanguage.googleapis.com";
}

String GeminiProvider::get_default_model() const {
	return "gemini-pro";
}

Dictionary GeminiProvider::build_request_body(const String &user_prompt, const String &context_block) const {
	Dictionary body;

	// Build contents array
	Array contents;
	
	Dictionary user_content;
	Array parts;
	Dictionary text_part;
	
	// Combine system prompt and user prompt for Gemini
	String system_content = get_system_prompt();
	if (!context_block.is_empty()) {
		system_content += context_block;
	}
	String combined_prompt = system_content + "\n\nUser request: " + user_prompt;
	text_part["text"] = combined_prompt;
	parts.push_back(text_part);
	
	user_content["parts"] = parts;
	contents.push_back(user_content);

	body["contents"] = contents;

	// Generation config
	Dictionary generation_config;
	generation_config["temperature"] = temperature;
	generation_config["maxOutputTokens"] = max_tokens;
	body["generationConfig"] = generation_config;

	return body;
}

String GeminiProvider::parse_response(const Dictionary &response_data) const {
	// Gemini response format: {"candidates": [{"content": {"parts": [{"text": "..."}]}}]}
	if (!response_data.has("candidates")) {
		ERR_PRINT("Gemini response missing 'candidates' field");
		return "";
	}

	Array candidates = response_data["candidates"];
	if (candidates.is_empty()) {
		ERR_PRINT("Gemini response 'candidates' array is empty");
		return "";
	}

	Dictionary first_candidate = candidates[0];
	if (!first_candidate.has("content")) {
		ERR_PRINT("Gemini response candidate missing 'content' field");
		return "";
	}

	Dictionary content = first_candidate["content"];
	if (!content.has("parts")) {
		ERR_PRINT("Gemini response content missing 'parts' field");
		return "";
	}

	Array parts = content["parts"];
	if (parts.is_empty()) {
		ERR_PRINT("Gemini response 'parts' array is empty");
		return "";
	}

	Dictionary first_part = parts[0];
	if (!first_part.has("text")) {
		ERR_PRINT("Gemini response part missing 'text' field");
		return "";
	}

	return first_part["text"];
}

PackedStringArray GeminiProvider::get_request_headers() const {
	PackedStringArray headers;
	headers.push_back("Content-Type: application/json");
	return headers;
}

String GeminiProvider::get_request_url() const {
	return base_url + "/v1beta/models/" + model + ":generateContent?key=" + api_key;
}

void GeminiProvider::send_request(const String &user_prompt, const String &context_block) {
	if (api_key.is_empty()) {
		ERR_PRINT("GeminiProvider::send_request() - API key is not set");
		call_deferred("emit_signal", "request_completed", false, "", "API key is not set");
		return;
	}
	
	print_line(vformat("GeminiProvider: Sending request to %s", get_request_url()));
	
	// Submit task to worker thread pool
	WorkerThreadPool::get_singleton()->add_task(
		callable_mp(this, &GeminiProvider::_perform_request).bind(user_prompt, context_block)
	);
}

void GeminiProvider::_perform_request(const String &user_prompt, const String &context_block) {
	HTTPClient *http_client = HTTPClient::create();
	
	String host = "generativelanguage.googleapis.com";
	int port = 443;
	
	// Connect to host
	Ref<TLSOptions> tls_options = TLSOptions::client();
	Error err = http_client->connect_to_host(host, port, tls_options);
	if (err != OK) {
		ERR_PRINT(vformat("GeminiProvider: Failed to connect to host: %d", err));
		call_deferred("emit_signal", "request_completed", false, "", vformat("Failed to connect: %d", err));
		memdelete(http_client);
		return;
	}
	
	// Wait for connection
	while (http_client->get_status() == HTTPClient::STATUS_CONNECTING ||
	       http_client->get_status() == HTTPClient::STATUS_RESOLVING) {
		http_client->poll();
		OS::get_singleton()->delay_usec(10000); // 10ms
	}
	
	if (http_client->get_status() != HTTPClient::STATUS_CONNECTED) {
		ERR_PRINT(vformat("GeminiProvider: Connection failed, status: %d", http_client->get_status()));
		call_deferred("emit_signal", "request_completed", false, "", "Connection failed");
		memdelete(http_client);
		return;
	}
	
	// Build request
	Dictionary request_body = build_request_body(user_prompt, context_block);
	String json_body = JSON::stringify(request_body);
	PackedStringArray headers_array = get_request_headers();
	
	Vector<String> headers_vector;
	for (int i = 0; i < headers_array.size(); i++) {
		headers_vector.push_back(headers_array[i]);
	}
	
	// Build URL path with model and API key
	String path = vformat("/v1beta/models/%s:generateContent?key=%s", model, api_key);
	
	// Send request
	CharString body_data = json_body.utf8();
	err = http_client->request(HTTPClient::METHOD_POST, path, headers_vector, (const uint8_t *)body_data.get_data(), body_data.length());
	if (err != OK) {
		ERR_PRINT(vformat("GeminiProvider: Failed to send request: %d", err));
		call_deferred("emit_signal", "request_completed", false, "", vformat("Failed to send request: %d", err));
		memdelete(http_client);
		return;
	}
	
	// Wait for response
	while (http_client->get_status() == HTTPClient::STATUS_REQUESTING) {
		http_client->poll();
		OS::get_singleton()->delay_usec(10000); // 10ms
	}
	
	if (http_client->get_status() != HTTPClient::STATUS_BODY &&
	    http_client->get_status() != HTTPClient::STATUS_CONNECTED) {
		ERR_PRINT(vformat("GeminiProvider: Request failed, status: %d", http_client->get_status()));
		call_deferred("emit_signal", "request_completed", false, "", "Request failed");
		memdelete(http_client);
		return;
	}
	
	// Check response code
	int response_code = http_client->get_response_code();
	if (response_code != 200) {
		ERR_PRINT(vformat("GeminiProvider: HTTP error code: %d", response_code));
		call_deferred("emit_signal", "request_completed", false, "", vformat("HTTP error: %d", response_code));
		memdelete(http_client);
		return;
	}
	
	// Read response body
	PackedByteArray response_body;
	while (http_client->get_status() == HTTPClient::STATUS_BODY) {
		http_client->poll();
		PackedByteArray chunk = http_client->read_response_body_chunk();
		if (chunk.size() > 0) {
			response_body.append_array(chunk);
		} else {
			OS::get_singleton()->delay_usec(10000); // 10ms
		}
	}
	
	String response_str = String::utf8((const char *)response_body.ptr(), response_body.size());
	
	// Parse JSON response
	JSON json_parser;
	err = json_parser.parse(response_str);
	if (err != Error::OK) {
		ERR_PRINT(vformat("GeminiProvider: Failed to parse response JSON: %s", json_parser.get_error_message()));
		call_deferred("emit_signal", "request_completed", false, "", "Failed to parse response JSON");
		memdelete(http_client);
		return;
	}
	
	Dictionary response_data = json_parser.get_data();
	String ai_response = parse_response(response_data);
	
	if (ai_response.is_empty()) {
		ERR_PRINT("GeminiProvider: Empty response from AI");
		call_deferred("emit_signal", "request_completed", false, "", "Empty response");
		memdelete(http_client);
		return;
	}
	
	print_line(vformat("GeminiProvider: Received response: %s", ai_response));
	call_deferred("emit_signal", "request_completed", true, ai_response, "");
	
	// Clean up
	memdelete(http_client);
}

// ============================================================================
// XAIProvider Implementation (x.ai / Grok)
// ============================================================================

XAIProvider::XAIProvider() : AIProvider() {
	model = get_default_model();
	base_url = get_default_base_url();
	
	// Try to load API key from environment
	String env_key = load_api_key_from_env("XAI_API_KEY");
	if (!env_key.is_empty()) {
		api_key = env_key;
	}
}

XAIProvider::~XAIProvider() {
}

void XAIProvider::_bind_methods() {
	// No additional methods to bind for now
}

String XAIProvider::get_default_base_url() const {
	return "https://api.x.ai";
}

String XAIProvider::get_default_model() const {
	return "grok-4-fast";
}

Dictionary XAIProvider::build_request_body(const String &user_prompt, const String &context_block) const {
	// x.ai uses OpenAI-compatible API
	Dictionary body;
	body["model"] = model;
	body["temperature"] = temperature;
	body["max_tokens"] = max_tokens;

	// Build messages array
	Array messages;
	
	Dictionary system_msg;
	system_msg["role"] = "system";
	String system_content = get_system_prompt();
	if (!context_block.is_empty()) {
		system_content += context_block;
	}
	system_msg["content"] = system_content;
	messages.push_back(system_msg);

	Dictionary user_msg;
	user_msg["role"] = "user";
	user_msg["content"] = user_prompt;
	messages.push_back(user_msg);

	body["messages"] = messages;

	return body;
}

String XAIProvider::parse_response(const Dictionary &response_data) const {
	// x.ai uses OpenAI-compatible response format
	if (!response_data.has("choices")) {
		ERR_PRINT("x.ai response missing 'choices' field");
		return "";
	}

	Array choices = response_data["choices"];
	if (choices.is_empty()) {
		ERR_PRINT("x.ai response 'choices' array is empty");
		return "";
	}

	Dictionary first_choice = choices[0];
	if (!first_choice.has("message")) {
		ERR_PRINT("x.ai response choice missing 'message' field");
		return "";
	}

	Dictionary message = first_choice["message"];
	if (!message.has("content")) {
		ERR_PRINT("x.ai response message missing 'content' field");
		return "";
	}

	return message["content"];
}

PackedStringArray XAIProvider::get_request_headers() const {
	PackedStringArray headers;
	headers.push_back("Content-Type: application/json");
	headers.push_back("Authorization: Bearer " + api_key);
	return headers;
}

String XAIProvider::get_request_url() const {
	return base_url + "/v1/chat/completions";
}

void XAIProvider::send_request(const String &user_prompt, const String &context_block) {
	if (api_key.is_empty()) {
		ERR_PRINT("XAIProvider::send_request() - API key is not set");
		call_deferred("emit_signal", "request_completed", false, "", "API key is not set");
		return;
	}
	
	print_line(vformat("XAIProvider: Sending request to %s", get_request_url()));
	
	// Submit task to worker thread pool
	WorkerThreadPool::get_singleton()->add_task(
		callable_mp(this, &XAIProvider::_perform_request).bind(user_prompt, context_block)
	);
}

void XAIProvider::_perform_request(const String &user_prompt, const String &context_block) {
	HTTPClient *http_client = HTTPClient::create();
	
	String host = "api.x.ai";
	int port = 443;
	
	// Connect to host
	Ref<TLSOptions> tls_options = TLSOptions::client();
	Error err = http_client->connect_to_host(host, port, tls_options);
	if (err != OK) {
		ERR_PRINT(vformat("XAIProvider: Failed to connect to host: %d", err));
		call_deferred("emit_signal", "request_completed", false, "", vformat("Failed to connect: %d", err));
		memdelete(http_client);
		return;
	}
	
	// Wait for connection
	while (http_client->get_status() == HTTPClient::STATUS_CONNECTING ||
	       http_client->get_status() == HTTPClient::STATUS_RESOLVING) {
		http_client->poll();
		OS::get_singleton()->delay_usec(10000); // 10ms
	}
	
	if (http_client->get_status() != HTTPClient::STATUS_CONNECTED) {
		ERR_PRINT(vformat("XAIProvider: Connection failed, status: %d", http_client->get_status()));
		call_deferred("emit_signal", "request_completed", false, "", "Connection failed");
		memdelete(http_client);
		return;
	}
	
	// Build request
	Dictionary request_body = build_request_body(user_prompt, context_block);
	String json_body = JSON::stringify(request_body);
	PackedStringArray headers_array = get_request_headers();
	
	Vector<String> headers_vector;
	for (int i = 0; i < headers_array.size(); i++) {
		headers_vector.push_back(headers_array[i]);
	}
	
	// Send request
	CharString body_data = json_body.utf8();
	err = http_client->request(HTTPClient::METHOD_POST, "/v1/chat/completions", headers_vector, (const uint8_t *)body_data.get_data(), body_data.length());
	if (err != OK) {
		ERR_PRINT(vformat("XAIProvider: Failed to send request: %d", err));
		call_deferred("emit_signal", "request_completed", false, "", vformat("Failed to send request: %d", err));
		memdelete(http_client);
		return;
	}
	
	// Wait for response
	while (http_client->get_status() == HTTPClient::STATUS_REQUESTING) {
		http_client->poll();
		OS::get_singleton()->delay_usec(10000); // 10ms
	}
	
	if (http_client->get_status() != HTTPClient::STATUS_BODY &&
	    http_client->get_status() != HTTPClient::STATUS_CONNECTED) {
		ERR_PRINT(vformat("XAIProvider: Request failed, status: %d", http_client->get_status()));
		call_deferred("emit_signal", "request_completed", false, "", "Request failed");
		memdelete(http_client);
		return;
	}
	
	// Check response code
	int response_code = http_client->get_response_code();
	if (response_code != 200) {
		ERR_PRINT(vformat("XAIProvider: HTTP error code: %d", response_code));
		call_deferred("emit_signal", "request_completed", false, "", vformat("HTTP error: %d", response_code));
		memdelete(http_client);
		return;
	}
	
	// Read response body
	PackedByteArray response_body;
	while (http_client->get_status() == HTTPClient::STATUS_BODY) {
		http_client->poll();
		PackedByteArray chunk = http_client->read_response_body_chunk();
		if (chunk.size() > 0) {
			response_body.append_array(chunk);
		} else {
			OS::get_singleton()->delay_usec(10000); // 10ms
		}
	}
	
	String response_str = String::utf8((const char *)response_body.ptr(), response_body.size());
	
	// Parse JSON response
	JSON json_parser;
	err = json_parser.parse(response_str);
	if (err != Error::OK) {
		ERR_PRINT(vformat("XAIProvider: Failed to parse response JSON: %s", json_parser.get_error_message()));
		call_deferred("emit_signal", "request_completed", false, "", "Failed to parse response JSON");
		memdelete(http_client);
		return;
	}
	
	Dictionary response_data = json_parser.get_data();
	String ai_response = parse_response(response_data);
	
	if (ai_response.is_empty()) {
		ERR_PRINT("XAIProvider: Empty response from AI");
		call_deferred("emit_signal", "request_completed", false, "", "Empty response");
		memdelete(http_client);
		return;
	}
	
	print_line(vformat("XAIProvider: Received response: %s", ai_response));
	call_deferred("emit_signal", "request_completed", true, ai_response, "");
	
	// Clean up
	memdelete(http_client);
}

// ============================================================================
// DummyProvider Implementation
// ============================================================================

DummyProvider::DummyProvider() : AIProvider() {
	model = get_default_model();
	base_url = get_default_base_url();
}

void DummyProvider::_bind_methods() {
	// No additional methods to bind for now
}

String DummyProvider::get_default_base_url() const {
	return "dummy://localhost";
}

String DummyProvider::get_default_model() const {
	return "dummy-model";
}

Dictionary DummyProvider::build_request_body(const String &user_prompt, const String &context_block) const {
	// Dummy provider doesn't need to build real requests
	Dictionary body;
	body["prompt"] = user_prompt;
	body["context"] = context_block;
	return body;
}

String DummyProvider::parse_response(const Dictionary &response_data) const {
	// This won't be called since we don't make HTTP requests
	return "";
}

PackedStringArray DummyProvider::get_request_headers() const {
	// Dummy provider doesn't need headers
	PackedStringArray headers;
	return headers;
}

String DummyProvider::get_request_url() const {
	return "dummy://localhost";
}

void DummyProvider::send_request(const String &user_prompt, const String &context_block) {
	// DummyProvider doesn't make real HTTP requests - just immediately return dummy data
	print_line("DummyProvider: Returning simulated response");
	String response = get_dummy_response(user_prompt);
	emit_signal("request_completed", true, response, "");
}

String DummyProvider::get_dummy_response(const String &user_prompt) const {
	// Simplified simulated response for testing
	return "[ \
        {\"action\": \"create_node\", \"args\": {\"node_name\": \"TestNode\", \"node_type\": \"Sprite2D\", \"parent_path\": \"\"}}, \
        {\"action\": \"set_property\", \"args\": {\"node_path\": \"TestNode\", \"property_name\": \"position\", \"value\": {\"x\": 50, \"y\": 50}}} \
    ]";
}

