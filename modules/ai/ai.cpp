// modules/ai/ai.cpp
#include "ai.h"

#include "core/core_bind.h"     // For ClassDB bindings (D_METHOD)
#include "core/error/error_macros.h" // For ERR_FAIL_* macros
#include "core/variant/variant.h" // For Variant type
#include "core/variant/dictionary.h" // Required for Dictionary
#include "core/io/json.h" // Required for JSON parsing
// #include "core/io/http_client.h" // Required for HTTPClient - Removed for now
// #include "core/crypto/crypto.h" // Required for TLSOptions (defined here) - Removed for now
// #include "main/main.h" // Required for Main::iteration() - Removed for now

// Define and initialize the static singleton pointer.
AI *AI::singleton = nullptr;

// Implementation for the main logic method.
Array AI::request_actions(const String &prompt) {
	print_verbose("AI::request_actions called with prompt: " + prompt);

	// --- Reverted to Test 2: JSON Parsing --- //
	// Hardcoded JSON string for testing
	String json_string = "[ \n" \
		"  { \"action\": \"create_node\", \"type\": \"Node2D\", \"name\": \"MyNode\" }, \n" \
		"  { \"action\": \"set_property\", \"node_path\": \"MyNode\", \"property\": \"position\", \"value\": [100, 50] }, \n" \
		"  { \"action\": \"call_method\", \"node_path\": \"MyNode\", \"method\": \"print_tree_pretty\" } \n" \
	"]";

	print_verbose("Attempting to parse JSON: " + json_string);

	// 1. Instantiate a JSON object directly
	Ref<JSON> json_parser;
	json_parser.instantiate(); // Equivalent to memnew(JSON) wrapped in Ref

	// Check if instantiation failed
	if (json_parser.is_null()) {
		print_error("JSON parsing failed: Unable to instantiate JSON object directly. Returning empty array.");
		return Array();
	} else {
		print_verbose("Successfully instantiated Ref<JSON> object.");
	}

	// 2. Call the parse method on the instance
	Error err = json_parser->parse(json_string); // Use the instance method

	// Check for parsing errors reported by the parser object
	if (err != OK) { // Check the return value of parse()
		// We can still get error details from the object even if parse returns != OK
		print_error("JSON parsing failed at line " + itos(json_parser->get_error_line()) + ": " + json_parser->get_error_message() + ". Returning empty array.");
		return Array(); // Return empty array on error
	}

	// Get the parsed data
	print_verbose("JSON parsing successful. Getting data...");
	Variant result = json_parser->get_data();

	// Check if the result is an Array
	if (result.get_type() != Variant::ARRAY) {
		print_error("Parsed JSON is not an Array (Type: " + Variant::get_type_name(result.get_type()) + "). Returning empty array.");
		return Array(); // Return empty array if not an array
	}

	print_verbose("Parsed data is an Array. Casting...");
	// Cast the result to Array (or you could validate dictionaries inside too)
	Array parsed_array = result;

	print_verbose("AI::request_actions returning parsed data: " + Variant(parsed_array).stringify());

	// TODO: Validate structure of Array<Dictionary>

	return parsed_array; // Return the parsed array
}

// Bind methods to be exposed to Godot's scripting and editor.
void AI::_bind_methods() {
	// Bind the request_actions method, making it callable as `AI.request_actions("your prompt")`.
	ClassDB::bind_method(D_METHOD("request_actions", "prompt"), &AI::request_actions);
}

// Singleton Management Implementations
void AI::initialize_singleton() {
	// Ensure singleton doesn't already exist.
	ERR_FAIL_COND_MSG(singleton != nullptr, "AI singleton already initialized.");
	singleton = memnew(AI);
}

void AI::finalize_singleton() {
	// Ensure singleton exists before deleting.
	ERR_FAIL_COND_MSG(singleton == nullptr, "AI singleton not initialized or already finalized.");
	memdelete(singleton);
	singleton = nullptr;
}

AI *AI::get_singleton() {
	// Optional: Could add ERR_FAIL_NULL_V(singleton, nullptr); here too,
	// but initialize/finalize should handle the lifecycle.
	return singleton;
}

// Constructor: Called by memnew in initialize_singleton.
AI::AI() {
	// Ensure the singleton pointer is correctly set during construction.
	ERR_FAIL_COND_MSG(singleton != nullptr && singleton != this, "AI singleton race condition detected.");
	// If initialize_singleton called memnew(AI), 'this' should be the value assigned to 'singleton'.
	// If singleton is null here, it means initialize_singleton hasn't run or is running concurrently.
}

// Destructor: Called by memdelete in finalize_singleton.
AI::~AI() {
	// Ensure the singleton pointer matches this instance upon destruction.
	ERR_FAIL_COND_MSG(singleton != this, "AI singleton pointer mismatch during destruction.");
	// If singleton is null here, it means finalize_singleton already ran or wasn't called.
} 