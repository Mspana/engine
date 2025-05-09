#ifndef AI_H
#define AI_H

#include "core/object/object.h"         // Base class
#include "core/variant/array.h"         // For Array return type
#include "core/string/ustring.h"        // For String parameter type
#include "core/variant/dictionary.h"    // Added for Dictionary type hint

class AI : public Object {
	GDCLASS(AI, Object); // Godot class macro

	static AI *singleton; // Pointer to the singleton instance

protected:
	// Binds methods to be used in scripting.
	static void _bind_methods();

	// Helper method for binding validate_command_json to GDScript.
	Dictionary _validate_command_json_bind(const String &json_str) const;

	// Validates the JSON string; on failure, returns false and populates error_msg.
    bool validate_command_json(const String &json_str, String &error_msg) const;

private:
	// Helper to validate a command already parsed into a Dictionary.
	bool _validate_command_dictionary(const Dictionary &cmd, String &error_msg) const;
	// Helper to simulate getting a JSON response string from an AI.
	String _get_simulated_ai_response_json_string(const String &user_prompt) const;

	// Execution helpers
	void _execute_create_node(const Dictionary &args);
	void _execute_set_property(const Dictionary &args);

public:
	// The core method to interact with the AI backend.
	Array request_actions(const String &prompt);

	// Static methods for singleton management (called from register_types.cpp)
	static void initialize_singleton();
	static void finalize_singleton();
	// Static getter for easy C++ access to the singleton.
	static AI *get_singleton();

	// Constructor and Destructor
	AI();
	~AI();
};

#endif // AI_H 