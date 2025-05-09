// modules/ai/ai.cpp
#include "ai.h"

#include "core/core_bind.h"     // For ClassDB bindings (D_METHOD)
#include "core/error/error_macros.h" // For ERR_FAIL_* macros
#include "core/variant/variant.h" // For Variant type
#include "core/variant/dictionary.h" // Required for Dictionary
#include "core/io/json.h" // Required for JSON parsing
#include "core/string/ustring.h" // For String utilities
// #include "core/io/json.cpp" // Generally not good practice to include .cpp files. Assuming JSON is linked.

// Headers for execution logic
#include "editor/editor_interface.h"
#include "editor/editor_undo_redo_manager.h" // For editor undo/redo
#include "core/object/class_db.h"
#include "scene/main/node.h"
#include "core/string/node_path.h"


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
        // parent_path is optional, can be string. If present and not string, it's an issue for get_node.
        if (args.has("parent_path") && args["parent_path"].get_type() != Variant::STRING) {
            error_msg = "'create_node' optional 'parent_path' must be a string.";
            return false;
        }
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
    // Simplified for testing execution
    return "[ \
        {\"action\": \"create_node\", \"args\": {\"node_name\": \"TestNode\", \"node_type\": \"Sprite2D\", \"parent_path\": \"\"}}, \
        {\"action\": \"set_property\", \"args\": {\"node_path\": \"TestNode\", \"property_name\": \"position\", \"value\": {\"x\": 50, \"y\": 50}}} \
    ]";
}

void AI::_execute_create_node(const Dictionary &args) {
    EditorInterface *ei = EditorInterface::get_singleton();
    if (!ei) {
        ERR_PRINT("AI Execute: EditorInterface singleton not found.");
        return;
    }

    EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
    if (!undo_redo) {
        ERR_PRINT("AI Execute: EditorUndoRedoManager singleton not found.");
        return;
    }
    
    Node *edited_scene_root = ei->get_edited_scene_root();
    if (!edited_scene_root) {
        ERR_PRINT("AI Execute 'create_node': No edited scene root.");
        return;
    }

    String node_name = args["node_name"];
    String node_type = args["node_type"];
    String parent_path_str = args.get("parent_path", ""); // Default to empty string if not present

    if (!ClassDB::class_exists(StringName(node_type))) {
        ERR_PRINT(vformat("AI Execute 'create_node': Node type '%s' does not exist.", node_type));
        return;
    }

    Node *parent_node = nullptr;
    if (parent_path_str.is_empty()) {
        parent_node = edited_scene_root;
    } else {
        parent_node = edited_scene_root->get_node(NodePath(parent_path_str));
        if (!parent_node) {
            ERR_PRINT(vformat("AI Execute 'create_node': Could not find parent node at path '%s'. Using scene root instead.", parent_path_str));
            parent_node = edited_scene_root; // Fallback or error
        }
    }
    if (!parent_node) { // Should be redundant if above fallback works, but as a safeguard.
        ERR_PRINT("AI Execute 'create_node': Failed to determine parent node.");
        return;
    }


    Node *new_node = Object::cast_to<Node>(ClassDB::instantiate(StringName(node_type)));
    if (!new_node) {
        ERR_PRINT(vformat("AI Execute 'create_node': Failed to instantiate node of type '%s'.", node_type));
        return;
    }
    // new_node->set_name(node_name); // Set name before adding for UndoRedo add_child if it relies on initial name for remove

    undo_redo->create_action("AI Create Node");
    // Add child first, then set name and owner for 'do'
    undo_redo->add_do_method(parent_node, "add_child", new_node, true); // force_readable_name = true
    undo_redo->add_do_method(new_node, "set_name", node_name);
    if (edited_scene_root != nullptr && new_node->get_owner() != edited_scene_root) { // Set owner if not already set (e.g. by add_child)
        undo_redo->add_do_method(new_node, "set_owner", edited_scene_root);
    }
    // For undo, remove child. The node itself will be freed if it has no other parent.
    undo_redo->add_undo_method(parent_node, "remove_child", new_node);
    // If new_node needs explicit freeing, add_undo_method(new_node, "queue_free") but usually remove_child handles this.
    undo_redo->commit_action();
    
    print_line(vformat("AI: Executed create_node. Name: %s, Type: %s, Parent: %s", node_name, node_type, parent_node->get_path()));
}

void AI::_execute_set_property(const Dictionary &args) {
    EditorInterface *ei = EditorInterface::get_singleton();
    if (!ei) {
        ERR_PRINT("AI Execute: EditorInterface singleton not found.");
        return;
    }
    EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
    if (!undo_redo) {
        ERR_PRINT("AI Execute: EditorUndoRedoManager singleton not found.");
        return;
    }
    Node *edited_scene_root = ei->get_edited_scene_root();
    if (!edited_scene_root) {
        ERR_PRINT("AI Execute 'set_property': No edited scene root.");
        return;
    }

    String node_path_str = args["node_path"];
    String property_name = args["property_name"];
    Variant value = args["value"];

    Node *target_node = edited_scene_root->get_node(NodePath(node_path_str));
    if (!target_node) {
        ERR_PRINT(vformat("AI Execute 'set_property': Could not find node at path '%s'.", node_path_str));
        return;
    }

    // It's good practice to check if property exists and is settable, though `set` might handle it.
    // For simplicity as per prompt, directly using set.
    // bool success = false;
    // target_node->set(property_name, value, &success);
    // if(!success) {
    //     ERR_PRINT(vformat("AI Execute 'set_property': Failed to set property '%s' on node '%s'. It might not exist or be read-only.", property_name, node_path_str));
    //     return;
    // }

    Variant current_value = target_node->get(property_name); // Get current value for undo

    undo_redo->create_action("AI Set Property");
    // Per user request, use target->set for do/undo methods
    undo_redo->add_do_method(target_node, "set", property_name, value);
    undo_redo->add_undo_method(target_node, "set", property_name, current_value);
    // Alternative using property methods:
    // undo_redo->add_do_property(target_node, property_name, value);
    // undo_redo->add_undo_property(target_node, property_name, current_value);
    undo_redo->commit_action();

    print_line(vformat("AI: Executed set_property. Node: %s, Property: %s, Value: %s", node_path_str, property_name, String(value)));
}

Array AI::request_actions(const String &user_prompt) {
    String ai_json_response = _get_simulated_ai_response_json_string(user_prompt);
    Array valid_actions_array;

    JSON json_parser;
    Error err = json_parser.parse(ai_json_response);
    if (err != Error::OK) {
        print_error(vformat("AI::request_actions - JSON Parse Error from AI response: %s at line %d. Raw response: %s", json_parser.get_error_message(), json_parser.get_error_line(), ai_json_response));
        return valid_actions_array;
    }

    Variant parsed_data = json_parser.get_data();

    if (parsed_data.get_type() == Variant::ARRAY) {
        Array commands_array = parsed_data;
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

    // Execute valid actions
    if (!valid_actions_array.is_empty()) {
        print_line(vformat("AI: Found %d valid actions. Executing...", valid_actions_array.size()));
        for (int i = 0; i < valid_actions_array.size(); ++i) {
            const Dictionary &action_dict = valid_actions_array[i]; // Assuming it's a Dictionary
            String action_name = action_dict.get("action", ""); // Default to empty if key missing
            Variant args_variant = action_dict.get("args", Dictionary()); // Default to empty dict

            if (args_variant.get_type() == Variant::DICTIONARY) {
                Dictionary action_args = args_variant;
                print_line(vformat("  - Attempting to execute Action %d: %s", i + 1, JSON::stringify(action_dict)));
                if (action_name == "create_node") {
                    _execute_create_node(action_args);
                } else if (action_name == "set_property") {
                    _execute_set_property(action_args);
                } else {
                    print_line(vformat("    - Action '%s' has no execution logic implemented.", action_name));
                }
            } else {
                WARN_PRINT(vformat("  - Action %d ('%s') has invalid 'args' type. Expected Dictionary, got %s. Skipping execution.", i+1, action_name, Variant::get_type_name(args_variant.get_type())));
            }
        }
    } else {
        // This case might happen if the AI returns an empty array [] or if all actions in the array were invalid.
        print_line("AI: Processed prompt. No valid actions to execute (or AI returned empty/invalid array).");
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
    if (singleton) { // Check before memdelete
        memdelete(singleton);
        singleton = nullptr;
    }
}

AI *AI::get_singleton() {
    return singleton;
}

AI::AI() {
    // ERR_FAIL_COND_MSG(singleton != nullptr && singleton != this, "AI singleton race condition detected.");
    // Singleton assignment is done in initialize_singleton
}

AI::~AI() {
    // ERR_FAIL_COND_MSG(singleton != this && singleton != nullptr , "AI singleton pointer mismatch during destruction.");
    // Singleton clearing is done in finalize_singleton
}
