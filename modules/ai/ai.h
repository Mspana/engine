#ifndef AI_H
#define AI_H

#include "core/object/object.h"         // Base class
#include "core/variant/array.h"         // For Array return type
#include "core/string/ustring.h"        // For String parameter type
#include "core/variant/dictionary.h"    // Added for Dictionary type hint
#include "ai_provider.h"                // For AIProvider types
#include "retrieval.h"                  // For RetrievalIndex

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
	// Provider for AI API calls
	Ref<AIProvider> provider;
	
	// Retrieval index for RAG-lite context
	Ref<RetrievalIndex> retrieval;
	
	// Callback for provider request completion
	void _on_provider_request_completed(bool success, const String &response_json, const String &error_message);
	
	// Helper to process and execute actions from JSON response
	void _process_and_execute_actions(const String &ai_json_response);
	
	// Helper to validate a command already parsed into a Dictionary.
	bool _validate_command_dictionary(const Dictionary &cmd, String &error_msg) const;

	// Helper to get active scene path
	String _get_active_scene_path() const;

	
	// File operation helpers for UndoRedo
	void _create_script_file(const String &abs_path, const String &content);
	void _write_script_file(const String &abs_path, const String &content);
	void _delete_script_file(const String &abs_path);
	void _rename_script_file(const String &old_abs_path, const String &new_abs_path);

public:
	// The core method to interact with the AI backend.
	Array request_actions(const String &prompt);

	// Provider management
	void set_provider(const Ref<AIProvider> &p_provider);
	Ref<AIProvider> get_provider() const;

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