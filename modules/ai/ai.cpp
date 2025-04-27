// modules/ai/ai.cpp
#include "ai.h"

#include "core/core_bind.h"     // For ClassDB bindings (D_METHOD)
#include "core/error/error_macros.h" // For ERR_FAIL_* macros
#include "core/variant/variant.h" // For Variant type
#include "core/variant/dictionary.h" // Required for Dictionary
#include "core/io/json.h" // Required for JSON parsing
#include "core/string/ustring.h" // For String utilities
#include "core/io/json.cpp" // Include JSON implementation


// Define and initialize the static singleton pointer.
AI *AI::singleton = nullptr;

static const Vector<String> ALLOWED_ACTIONS = {
    "create_node","delete_node","set_property",
    "create_script","update_script","attach_script",
    "connect_signal","run_project","list_nodes","list_files"
};

// New helper function to validate a command already parsed into a Dictionary
bool AI::_validate_command_dictionary(const Dictionary &cmd, String &error_msg) const {
    // Check "action"
    if (!cmd.has("action") || !cmd["action"].is_string()) {
        error_msg = "Missing or non-string 'action' field.";
        return false;
    }
    String action = cmd["action"];
    if (!ALLOWED_ACTIONS.has(action)) {
        error_msg = vformat("Unknown action '%s'.", action);
        return false;
    }

    // Check "args"
    if (!cmd.has("args") || cmd["args"].get_type() != Variant::DICTIONARY) {
        error_msg = "Missing or non-object 'args' field.";
        return false;
    }
    Dictionary args = cmd["args"];

    // Per-action argument checks
    if (action == "create_node") {
        if (!args.has("node_name") || args["node_name"].get_type() != Variant::STRING) {
            error_msg = "'create_node' requires string 'node_name'.";
            return false;
        }
        if (!args.has("node_type") || args["node_type"].get_type() != Variant::STRING) {
            error_msg = "'create_node' requires string 'node_type'.";
            return false;
        }
        // parent_path optional
    } else if (action == "delete_node") {
        if (!args.has("node_path") || args["node_path"].get_type() != Variant::STRING) {
            error_msg = "'delete_node' requires string 'node_path'.";
            return false;
        }
    } else if (action == "set_property") {
        if (!args.has("node_path") || args["node_path"].get_type() != Variant::STRING) {
            error_msg = "'set_property' requires string 'node_path'.";
            return false;
        }
        if (!args.has("property_name") || args["property_name"].get_type() != Variant::STRING) {
            error_msg = "'set_property' requires string 'property_name'.";
            return false;
        }
        if (!args.has("value")) {
            error_msg = "'set_property' requires 'value'.";
            return false;
        }
    }
    // Additional actions can be validated similarly...

    return true;
}

bool AI::validate_command_json(const String &json_str, String &error_msg) const {
    JSON json_parser;
    Error err = json_parser.parse(json_str); // Call parse on the instance, get Error return
    if (err != Error::OK) {
        error_msg = vformat("JSON Parse Error: %s at line %d", json_parser.get_error_message(), json_parser.get_error_line());
        return false;
    }
    Variant top = json_parser.get_data(); // Get parsed data
    if (top.get_type() != Variant::DICTIONARY) {
        error_msg = "Top-level JSON is not an object.";
        return false;
    }
    Dictionary cmd = top;
    return _validate_command_dictionary(cmd, error_msg);
}

// New private method to simulate getting a response from an AI
String AI::_get_simulated_ai_response_json_string(const String &user_prompt) const {
    // For now, user_prompt is ignored and we return a hardcoded JSON string.
    // This simulates the AI returning an array with a single valid create_node action.
    return "[ \
        {\"action\": \"create_node\", \"args\": {\"node_name\": \"MySpriteFromAI\", \"node_type\": \"Sprite2D\", \"parent_path\": \"/root/MainScene\"}} \
    ]";
}

Array AI::request_actions(const String &user_prompt) {
    String ai_json_response = _get_simulated_ai_response_json_string(user_prompt);
    Array valid_actions_array;

    JSON json_parser;
    Error err = json_parser.parse(ai_json_response);
    if (err != Error::OK) {
        print_error(vformat("AI::request_actions - JSON Parse Error from AI response: %s at line %d. Raw response: %s", json_parser.get_error_message(), json_parser.get_error_line(), ai_json_response));
        return valid_actions_array; // Return empty array
    }

    Variant parsed_data = json_parser.get_data();

    if (parsed_data.get_type() == Variant::ARRAY) {
        Array commands_array = parsed_data; // It must be an array.

        for (int i = 0; i < commands_array.size(); ++i) {
            if (commands_array[i].get_type() == Variant::DICTIONARY) {
                Dictionary command_dict = commands_array[i];
                String validation_error_msg;
                if (_validate_command_dictionary(command_dict, validation_error_msg)) {
                    valid_actions_array.push_back(command_dict);
                } else {
                    WARN_PRINT(vformat("AI::request_actions - Invalid command in AI response array at index %d: %s. Command: %s", i, validation_error_msg, JSON::stringify(commands_array[i])));
                }
            } else {
                // This element in the array was not a Dictionary.
                WARN_PRINT(vformat("AI::request_actions - Expected a command Dictionary in AI response array at index %d, got %s.", i, Variant::get_type_name(commands_array[i].get_type())));
            }
        }
    } else {
        // The AI response was not an array, which violates our assumption.
        print_error(vformat("AI::request_actions - AI response JSON is not an Array as expected. Got %s. Raw response: %s", Variant::get_type_name(parsed_data.get_type()), ai_json_response));
        // valid_actions_array is already empty, so we just fall through to return it.
    }

    // Print out the valid actions that would be "taken"
    if (!valid_actions_array.is_empty()) {
        print_line("AI: Processed prompt. Valid actions to take:");
        for (int i = 0; i < valid_actions_array.size(); ++i) {
            // We know these are Dictionaries because we only added valid ones.
            Dictionary action_to_take = valid_actions_array[i];
            print_line(vformat("  - Action %d: %s", i + 1, JSON::stringify(action_to_take)));
        }
    } else {
        // This case might happen if the AI returns an empty array [] or if all actions in the array were invalid.
        print_line("AI: Processed prompt. No valid actions to take (or AI returned empty array).");
    }

    return valid_actions_array;
}

// Helper method implementation for GDScript binding
Dictionary AI::_validate_command_json_bind(const String &json_str) const {
    String error_msg;
    bool is_valid = validate_command_json(json_str, error_msg);
    Dictionary result;
    result["valid"] = is_valid;
    result["error"] = error_msg;
    return result;
}

void AI::_bind_methods() {
    // The D_METHOD for request_actions now conceptually takes a "user_prompt"
    ClassDB::bind_method(D_METHOD("request_actions", "user_prompt"), &AI::request_actions);
    // Bind the helper method under the desired name for GDScript
    ClassDB::bind_method(D_METHOD("validate_command_json", "json_str"), &AI::_validate_command_json_bind);
}

void AI::initialize_singleton() {
    ERR_FAIL_COND_MSG(singleton != nullptr, "AI singleton already initialized.");
    singleton = memnew(AI);
}

void AI::finalize_singleton() {
    ERR_FAIL_COND_MSG(singleton == nullptr, "AI singleton not initialized or already finalized.");
    memdelete(singleton);
    singleton = nullptr;
}

AI *AI::get_singleton() {
    return singleton;
}

AI::AI() {
    ERR_FAIL_COND_MSG(singleton != nullptr && singleton != this, "AI singleton race condition detected.");
}

AI::~AI() {
    ERR_FAIL_COND_MSG(singleton != this, "AI singleton pointer mismatch during destruction.");
}
