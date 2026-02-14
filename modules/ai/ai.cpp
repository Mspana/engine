// modules/ai/ai.cpp
#include "ai.h"
#include "ai_provider.h"
#include "retrieval.h"

// Action implementations
#include "actions/node_actions.h"
#include "actions/script_actions.h"
#include "actions/scene_actions.h"
#include "actions/read_actions.h"
#include "actions/signal_actions.h"
#include "actions/project_actions.h"

#include "core/core_bind.h"     // For ClassDB bindings (D_METHOD)
#include "core/error/error_macros.h" // For ERR_FAIL_* macros
#include "core/variant/variant.h" // For Variant type
#include "core/variant/dictionary.h" // Required for Dictionary
#include "core/io/json.h" // Required for JSON parsing
#include "core/string/ustring.h" // For String utilities
#include "core/config/project_settings.h" // For ProjectSettings

// Headers for execution logic
#include "editor/editor_interface.h"
#include "editor/editor_undo_redo_manager.h" // For editor undo/redo
#include "editor/editor_file_system.h" // For filesystem refresh
#include "core/object/class_db.h"
#include "scene/main/node.h"
#include "core/string/node_path.h"

// Headers for file operations
#include "core/io/file_access.h"
#include "core/io/dir_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "modules/gdscript/gdscript.h"


// Define and initialize the static singleton pointer.
AI *AI::singleton = nullptr;

static const Vector<String> ALLOWED_ACTIONS = {
    "create_node","delete_node","duplicate_node","set_property",
    "create_script","update_script","attach_script","detach_script","rename_script","delete_script",
    "connect_signal","disconnect_signal","run_project","play_test",
    "rename_node","reparent_node","create_scene","open_scene","save_scene","close_scene","set_main_scene",
    "get_node_info","find_nodes_by_type","list_nodes","list_files","read_script","set_project_setting","get_project_settings","create_autoload_singleton","remove_autoload_singleton","import_asset","delete_asset",
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
    } else if (action == "duplicate_node") {
        if (!args.has("node_path") || args["node_path"].get_type() != Variant::STRING) {
            error_msg = "'duplicate_node' requires string 'node_path'.";
            return false;
        }
        // new_name is optional string
        if (args.has("new_name") && args["new_name"].get_type() != Variant::STRING) {
            error_msg = "'duplicate_node' optional 'new_name' must be a string.";
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
    } else if (action == "create_script") {
        if (!args.has("file_path") || args["file_path"].get_type() != Variant::STRING) {
            error_msg = "'create_script' requires string 'file_path'.";
            return false;
        }
        if (!args.has("language") || args["language"].get_type() != Variant::STRING) {
            error_msg = "'create_script' requires string 'language'.";
            return false;
        }
        if (!args.has("content") || args["content"].get_type() != Variant::STRING) {
            error_msg = "'create_script' requires string 'content'.";
            return false;
        }
    } else if (action == "update_script") {
        if (!args.has("file_path") || args["file_path"].get_type() != Variant::STRING) {
            error_msg = "'update_script' requires string 'file_path'.";
            return false;
        }
        if (!args.has("patch") || args["patch"].get_type() != Variant::STRING) {
            error_msg = "'update_script' requires string 'patch'.";
            return false;
        }
    } else if (action == "attach_script") {
        if (!args.has("node_path") || args["node_path"].get_type() != Variant::STRING) {
            error_msg = "'attach_script' requires string 'node_path'.";
            return false;
        }
        if (!args.has("script_path") || args["script_path"].get_type() != Variant::STRING) {
            error_msg = "'attach_script' requires string 'script_path'.";
            return false;
        }
    } else if (action == "detach_script") {
        if (!args.has("node_path") || args["node_path"].get_type() != Variant::STRING) {
            error_msg = "'detach_script' requires string 'node_path'.";
            return false;
        }
    } else if (action == "rename_script") {
        if (!args.has("old_path") || args["old_path"].get_type() != Variant::STRING) {
            error_msg = "'rename_script' requires string 'old_path'.";
            return false;
        }
        if (!args.has("new_path") || args["new_path"].get_type() != Variant::STRING) {
            error_msg = "'rename_script' requires string 'new_path'.";
            return false;
        }
    } else if (action == "delete_script") {
        if (!args.has("file_path") || args["file_path"].get_type() != Variant::STRING) {
            error_msg = "'delete_script' requires string 'file_path'.";
            return false;
        }
        // detach_from_nodes is optional bool
        if (args.has("detach_from_nodes") && args["detach_from_nodes"].get_type() != Variant::BOOL) {
            error_msg = "'delete_script' optional 'detach_from_nodes' must be a bool.";
            return false;
        }
    } else if (action == "rename_node") {
        if (!args.has("node_path") || args["node_path"].get_type() != Variant::STRING) {
            error_msg = "'rename_node' requires string 'node_path'.";
            return false;
        }
        if (!args.has("new_name") || args["new_name"].get_type() != Variant::STRING) {
            error_msg = "'rename_node' requires string 'new_name'.";
            return false;
        }
    } else if (action == "reparent_node") {
        if (!args.has("node_path") || args["node_path"].get_type() != Variant::STRING) {
            error_msg = "'reparent_node' requires string 'node_path'.";
            return false;
        }
        if (!args.has("new_parent_path") || args["new_parent_path"].get_type() != Variant::STRING) {
            error_msg = "'reparent_node' requires string 'new_parent_path'.";
            return false;
        }
        // index is optional int
    } else if (action == "create_scene") {
        if (!args.has("scene_path") || args["scene_path"].get_type() != Variant::STRING) {
            error_msg = "'create_scene' requires string 'scene_path'.";
            return false;
        }
        // root_type and root_name are optional strings
        if (args.has("root_type") && args["root_type"].get_type() != Variant::STRING) {
            error_msg = "'create_scene' optional 'root_type' must be a string.";
            return false;
        }
        if (args.has("root_name") && args["root_name"].get_type() != Variant::STRING) {
            error_msg = "'create_scene' optional 'root_name' must be a string.";
            return false;
        }
    } else if (action == "open_scene") {
        if (!args.has("scene_path") || args["scene_path"].get_type() != Variant::STRING) {
            error_msg = "'open_scene' requires string 'scene_path'.";
            return false;
        }
    } else if (action == "save_scene") {
        // No required args; optional scene_path is ignored
    } else if (action == "set_main_scene") {
        if (!args.has("scene_path") || args["scene_path"].get_type() != Variant::STRING) {
            error_msg = "'set_main_scene' requires string 'scene_path'.";
            return false;
        }
    } else if (action == "close_scene") {
        // save_if_modified is optional bool
        if (args.has("save_if_modified") && args["save_if_modified"].get_type() != Variant::BOOL) {
            error_msg = "'close_scene' optional 'save_if_modified' must be a bool.";
            return false;
        }
    } else if (action == "set_project_setting") {
        if (!args.has("key") || args["key"].get_type() != Variant::STRING) {
            error_msg = "'set_project_setting' requires string 'key'.";
            return false;
        }
        if (!args.has("value")) {
            error_msg = "'set_project_setting' requires 'value'.";
            return false;
        }
    } else if (action == "get_project_settings") {
        // prefix is optional string
        if (args.has("prefix") && args["prefix"].get_type() != Variant::STRING) {
            error_msg = "'get_project_settings' optional 'prefix' must be a string.";
            return false;
        }
        // keys is optional Array
        if (args.has("keys") && args["keys"].get_type() != Variant::ARRAY) {
            error_msg = "'get_project_settings' optional 'keys' must be an array.";
            return false;
        }
        // include_defaults is optional bool
        if (args.has("include_defaults") && args["include_defaults"].get_type() != Variant::BOOL) {
            error_msg = "'get_project_settings' optional 'include_defaults' must be a bool.";
            return false;
        }
    } else if (action == "create_autoload_singleton") {
        if (!args.has("name") || args["name"].get_type() != Variant::STRING) {
            error_msg = "'create_autoload_singleton' requires string 'name'.";
            return false;
        }
        if (!args.has("script_path") || args["script_path"].get_type() != Variant::STRING) {
            error_msg = "'create_autoload_singleton' requires string 'script_path'.";
            return false;
        }
        // enabled is optional bool
        if (args.has("enabled") && args["enabled"].get_type() != Variant::BOOL) {
            error_msg = "'create_autoload_singleton' optional 'enabled' must be a bool.";
            return false;
        }
    } else if (action == "remove_autoload_singleton") {
        if (!args.has("name") || args["name"].get_type() != Variant::STRING) {
            error_msg = "'remove_autoload_singleton' requires string 'name'.";
            return false;
        }
    } else if (action == "import_asset") {
        if (!args.has("source_path") || args["source_path"].get_type() != Variant::STRING) {
            error_msg = "'import_asset' requires string 'source_path'.";
            return false;
        }
        if (!args.has("dest_path") || args["dest_path"].get_type() != Variant::STRING) {
            error_msg = "'import_asset' requires string 'dest_path'.";
            return false;
        }
        // overwrite is optional bool
        if (args.has("overwrite") && args["overwrite"].get_type() != Variant::BOOL) {
            error_msg = "'import_asset' optional 'overwrite' must be a bool.";
            return false;
        }
    } else if (action == "delete_asset") {
        if (!args.has("asset_path") || args["asset_path"].get_type() != Variant::STRING) {
            error_msg = "'delete_asset' requires string 'asset_path'.";
            return false;
        }
    } else if (action == "run_project" || action == "play_test") {
        // mode is optional string
        if (args.has("mode") && args["mode"].get_type() != Variant::STRING) {
            error_msg = "'run_project' optional 'mode' must be a string.";
            return false;
        }
        // scene_path is optional string
        if (args.has("scene_path") && args["scene_path"].get_type() != Variant::STRING) {
            error_msg = "'run_project' optional 'scene_path' must be a string.";
            return false;
        }
    } else if (action == "list_nodes") {
        // root_path is optional string
        if (args.has("root_path") && args["root_path"].get_type() != Variant::STRING) {
            error_msg = "'list_nodes' optional 'root_path' must be a string.";
            return false;
        }
    } else if (action == "get_node_info") {
        if (!args.has("node_path") || args["node_path"].get_type() != Variant::STRING) {
            error_msg = "'get_node_info' requires string 'node_path'.";
            return false;
        }
    } else if (action == "find_nodes_by_type") {
        if (!args.has("type_name") || args["type_name"].get_type() != Variant::STRING) {
            error_msg = "'find_nodes_by_type' requires string 'type_name'.";
            return false;
        }
    } else if (action == "list_files") {
        if (!args.has("directory") || args["directory"].get_type() != Variant::STRING) {
            error_msg = "'list_files' requires string 'directory'.";
            return false;
        }
        // glob is optional string
        if (args.has("glob") && args["glob"].get_type() != Variant::STRING) {
            error_msg = "'list_files' optional 'glob' must be a string.";
            return false;
        }
    } else if (action == "read_script") {
        if (!args.has("file_path") || args["file_path"].get_type() != Variant::STRING) {
            error_msg = "'read_script' requires string 'file_path'.";
            return false;
        }
    } else if (action == "connect_signal") {
        if (!args.has("emitter_path") || args["emitter_path"].get_type() != Variant::STRING) {
            error_msg = "'connect_signal' requires string 'emitter_path'.";
            return false;
        }
        if (!args.has("signal_name") || args["signal_name"].get_type() != Variant::STRING) {
            error_msg = "'connect_signal' requires string 'signal_name'.";
            return false;
        }
        if (!args.has("target_path") || args["target_path"].get_type() != Variant::STRING) {
            error_msg = "'connect_signal' requires string 'target_path'.";
            return false;
        }
        if (!args.has("method_name") || args["method_name"].get_type() != Variant::STRING) {
            error_msg = "'connect_signal' requires string 'method_name'.";
            return false;
        }
        // binds is optional Array
        if (args.has("binds") && args["binds"].get_type() != Variant::ARRAY) {
            error_msg = "'connect_signal' optional 'binds' must be an array.";
            return false;
        }
        // flags is optional int
        if (args.has("flags") && args["flags"].get_type() != Variant::INT) {
            error_msg = "'connect_signal' optional 'flags' must be an int.";
            return false;
        }
    } else if (action == "disconnect_signal") {
        if (!args.has("emitter_path") || args["emitter_path"].get_type() != Variant::STRING) {
            error_msg = "'disconnect_signal' requires string 'emitter_path'.";
            return false;
        }
        if (!args.has("signal_name") || args["signal_name"].get_type() != Variant::STRING) {
            error_msg = "'disconnect_signal' requires string 'signal_name'.";
            return false;
        }
        if (!args.has("target_path") || args["target_path"].get_type() != Variant::STRING) {
            error_msg = "'disconnect_signal' requires string 'target_path'.";
            return false;
        }
        if (!args.has("method_name") || args["method_name"].get_type() != Variant::STRING) {
            error_msg = "'disconnect_signal' requires string 'method_name'.";
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

Dictionary AI::_execute_single_action_internal(const Dictionary &p_action) {
    // Internal method - just delegates to the public execute_single_action
    return execute_single_action(p_action);
}

Dictionary AI::execute_single_action(const Dictionary &p_action) {
    // Extract action name and args from the action dictionary
    if (!p_action.has("action") || p_action["action"].get_type() != Variant::STRING) {
        Dictionary error_result;
        error_result["status"] = "error";
        Dictionary error_dict;
        error_dict["code"] = "invalid_action";
        error_dict["message"] = "Action dictionary missing 'action' field or it's not a string";
        error_dict["details"] = Dictionary();
        error_result["error"] = error_dict;
        return error_result;
    }

    if (!p_action.has("args") || p_action["args"].get_type() != Variant::DICTIONARY) {
        Dictionary error_result;
        error_result["status"] = "error";
        Dictionary error_dict;
        error_dict["code"] = "invalid_action";
        error_dict["message"] = "Action dictionary missing 'args' field or it's not a dictionary";
        error_dict["details"] = Dictionary();
        error_result["error"] = error_dict;
        return error_result;
    }

    String action_name = p_action["action"];
    Dictionary action_args = p_action["args"];

    // Dispatch to appropriate action handler based on action_name
    if (action_name == "create_node") {
        return AINodeActions::exec_create_node(action_args);
    } else if (action_name == "set_property") {
        return AINodeActions::exec_set_property(action_args);
    } else if (action_name == "rename_node") {
        return AINodeActions::exec_rename_node(action_args);
    } else if (action_name == "reparent_node") {
        return AINodeActions::exec_reparent_node(action_args);
    } else if (action_name == "delete_node") {
        return AINodeActions::exec_delete_node(action_args);
    } else if (action_name == "duplicate_node") {
        return AINodeActions::exec_duplicate_node(action_args);
    } else if (action_name == "create_script") {
        return AIScriptActions::exec_create_script(action_args);
    } else if (action_name == "update_script") {
        return AIScriptActions::exec_update_script(action_args);
    } else if (action_name == "attach_script") {
        return AIScriptActions::exec_attach_script(action_args);
    } else if (action_name == "detach_script") {
        return AIScriptActions::exec_detach_script(action_args);
    } else if (action_name == "rename_script") {
        return AIScriptActions::exec_rename_script(action_args);
    } else if (action_name == "delete_script") {
        return AIScriptActions::exec_delete_script(action_args);
    } else if (action_name == "create_scene") {
        return AISceneActions::exec_create_scene(action_args);
    } else if (action_name == "open_scene") {
        return AISceneActions::exec_open_scene(action_args);
    } else if (action_name == "save_scene") {
        return AISceneActions::exec_save_scene(action_args);
    } else if (action_name == "set_main_scene") {
        return AISceneActions::exec_set_main_scene(action_args);
    } else if (action_name == "close_scene") {
        return AISceneActions::exec_close_scene(action_args);
    } else if (action_name == "set_project_setting") {
        return AIProjectActions::exec_set_project_setting(action_args);
    } else if (action_name == "get_project_settings") {
        return AIProjectActions::exec_get_project_settings(action_args);
    } else if (action_name == "create_autoload_singleton") {
        return AIProjectActions::exec_create_autoload_singleton(action_args);
    } else if (action_name == "remove_autoload_singleton") {
        return AIProjectActions::exec_remove_autoload_singleton(action_args);
    } else if (action_name == "import_asset") {
        return AIProjectActions::exec_import_asset(action_args);
    } else if (action_name == "delete_asset") {
        return AIProjectActions::exec_delete_asset(action_args);
    } else if (action_name == "run_project" || action_name == "play_test") {
        return AIProjectActions::exec_run_project(action_args);
    } else if (action_name == "list_nodes") {
        return AIReadActions::exec_list_nodes(action_args);
    } else if (action_name == "get_node_info") {
        return AIReadActions::exec_get_node_info(action_args);
    } else if (action_name == "find_nodes_by_type") {
        return AIReadActions::exec_find_nodes_by_type(action_args);
    } else if (action_name == "list_files") {
        return AIReadActions::exec_list_files(action_args);
    } else if (action_name == "read_script") {
        return AIReadActions::exec_read_script(action_args);
    } else if (action_name == "connect_signal") {
        return AISignalActions::exec_connect_signal(action_args);
    } else if (action_name == "disconnect_signal") {
        return AISignalActions::exec_disconnect_signal(action_args);
    } else {
        // Unknown action
        Dictionary error_result;
        error_result["status"] = "error";
        Dictionary error_dict;
        error_dict["code"] = "unknown_action";
        error_dict["message"] = vformat("Unknown action: %s", action_name);
        error_dict["details"] = Dictionary();
        error_result["error"] = error_dict;
        return error_result;
    }
}

void AI::_on_provider_request_completed(bool success, const String &response_json, const String &error_message) {
    // If orchestrator is running, it handles responses - skip this legacy handler
    if (orchestrator.is_valid() && orchestrator->is_running()) {
        print_verbose("AI: Skipping legacy handler - orchestrator is running");
        return;
    }

    if (!success) {
        ERR_PRINT(vformat("AI::_on_provider_request_completed - Request failed: %s", error_message));
        return;
    }

    print_line("AI: Provider request completed successfully");
    _process_and_execute_actions(response_json);
}

void AI::_process_and_execute_actions(const String &ai_json_response) {
    Array valid_actions_array;

    JSON action_parser;
    Error err = action_parser.parse(ai_json_response);
    if (err != Error::OK) {
        ERR_PRINT(vformat("AI::_process_and_execute_actions - Failed to parse AI action JSON: %s. Response: %s", action_parser.get_error_message(), ai_json_response));
        return;
    }

    Variant parsed_data = action_parser.get_data();
    Array commands_array;

    // Handle new format: {"message": "...", "actions": [...]}
    if (parsed_data.get_type() == Variant::DICTIONARY) {
        Dictionary response_dict = parsed_data;
        
        // Log the message if present
        if (response_dict.has("message")) {
            String message = response_dict.get("message", "");
            if (!message.is_empty()) {
                print_line(vformat("AI Message: %s", message));
            }
        }
        
        // Extract actions array
        if (response_dict.has("actions") && response_dict["actions"].get_type() == Variant::ARRAY) {
            commands_array = response_dict["actions"];
        } else {
            print_verbose("AI: No actions in response.");
        }
    }
    // Handle legacy format: [{...}, {...}] (array of actions directly)
    else if (parsed_data.get_type() == Variant::ARRAY) {
        commands_array = parsed_data;
    } else {
        ERR_PRINT(vformat("AI::_process_and_execute_actions - AI response is not a Dictionary or Array. Got %s. Response: %s", Variant::get_type_name(parsed_data.get_type()), ai_json_response));
        return;
    }

    // Validate each action
    for (int i = 0; i < commands_array.size(); ++i) {
        if (commands_array[i].get_type() == Variant::DICTIONARY) {
            Dictionary command_dict = commands_array[i];
            String validation_error_msg;
            if (_validate_command_dictionary(command_dict, validation_error_msg)) {
                valid_actions_array.push_back(command_dict);
            } else {
                WARN_PRINT(vformat("AI::_process_and_execute_actions - Invalid command at index %d: %s. Command: %s", i, validation_error_msg, JSON::stringify(commands_array[i])));
            }
        } else {
            WARN_PRINT(vformat("AI::_process_and_execute_actions - Expected Dictionary at index %d, got %s.", i, Variant::get_type_name(commands_array[i].get_type())));
        }
    }

    // Execute valid actions
    if (!valid_actions_array.is_empty()) {
        print_line(vformat("AI: Received %d valid actions. Executing...", valid_actions_array.size()));
        for (int i = 0; i < valid_actions_array.size(); ++i) {
            const Dictionary &action_dict = valid_actions_array[i];
            String action_name = action_dict.get("action", "");

            print_line(vformat("  - Executing Action %d: %s", i + 1, JSON::stringify(action_dict)));

            // Execute action and get result
            Dictionary result = execute_single_action(action_dict);

            // Log result status
            String status = result.get("status", "unknown");
            if (status == "success") {
                print_verbose(vformat("    - Action '%s' succeeded", action_name));
            } else if (status == "error") {
                Dictionary error = result.get("error", Dictionary());
                String error_msg = error.get("message", "Unknown error");
                WARN_PRINT(vformat("    - Action '%s' failed: %s", action_name, error_msg));
            }
        }
    } else {
        print_line("AI: No valid actions to execute.");
    }
}

String AI::_get_active_scene_path() const {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return "";
	}
	
	Node *edited_scene_root = ei->get_edited_scene_root();
	if (!edited_scene_root) {
		return "";
	}
	
	// Get the scene file path
	String scene_file_path = edited_scene_root->get_scene_file_path();
	return scene_file_path;
}

Array AI::request_actions(const String &user_prompt) {
	Array result_array;

	// Check if provider is set
	if (provider.is_null()) {
		ERR_PRINT("AI::request_actions - No provider set. Use DummyProvider for testing or set a real provider.");
		return result_array;
	}

	// Get active scene path for context retrieval
	String active_scene_path = _get_active_scene_path();
	
	// Retrieve relevant project context
	Array context_snippets;
	if (retrieval.is_valid()) {
		context_snippets = retrieval->retrieve_context(user_prompt, active_scene_path, 8);
	}
	
	// Build context block from snippets
	String context_block;
	if (!context_snippets.is_empty()) {
		context_block = "\n\nProject context:\n";
		for (int i = 0; i < context_snippets.size(); i++) {
			Dictionary snippet = context_snippets[i];
			String file_path = snippet.get("file_path", "");
			String text = snippet.get("text", "");
			real_t score = snippet.get("score", 0.0);
			
			context_block += vformat("\n--- File: %s (relevance: %.1f) ---\n%s\n", file_path, score, text);
		}
		print_line(vformat("AI: Retrieved %d context snippets for prompt enrichment", context_snippets.size()));
	}

	print_line(vformat("AI: Sending request via %s provider", provider->get_class()));
	
	// Ask the provider to send the request with user prompt and context block
	// The provider will emit the "request_completed" signal when done
	provider->send_request(user_prompt, context_block);
	
	// For async providers (OpenAI, Gemini, XAI), actions will be executed when signal fires
	// For DummyProvider, the signal fires immediately and actions execute synchronously
	
	return result_array;
}

Array AI::request_actions_with_history(const Array &p_messages) {
	Array result_array;

	// Check if provider is set
	if (provider.is_null()) {
		ERR_PRINT("AI::request_actions_with_history - No provider set. Use DummyProvider for testing or set a real provider.");
		return result_array;
	}

	// Get active scene path for context retrieval
	String active_scene_path = _get_active_scene_path();
	
	// Extract the last user message for context retrieval
	String last_user_prompt;
	for (int i = p_messages.size() - 1; i >= 0; i--) {
		Dictionary msg = p_messages[i];
		if (msg.get("role", "") == "user") {
			last_user_prompt = msg.get("content", "");
			break;
		}
	}
	
	// Retrieve relevant project context based on the last user message
	Array context_snippets;
	if (retrieval.is_valid() && !last_user_prompt.is_empty()) {
		context_snippets = retrieval->retrieve_context(last_user_prompt, active_scene_path, 8);
	}
	
	// Build context block from snippets
	String context_block;
	if (!context_snippets.is_empty()) {
		context_block = "\n\nProject context:\n";
		for (int i = 0; i < context_snippets.size(); i++) {
			Dictionary snippet = context_snippets[i];
			String file_path = snippet.get("file_path", "");
			String text = snippet.get("text", "");
			real_t score = snippet.get("score", 0.0);
			
			context_block += vformat("\n--- File: %s (relevance: %.1f) ---\n%s\n", file_path, score, text);
		}
		print_line(vformat("AI: Retrieved %d context snippets for prompt enrichment", context_snippets.size()));
	}

	print_line(vformat("AI: Sending request with %d messages via %s provider", p_messages.size(), provider->get_class()));
	
	// Ask the provider to send the request with full message history and context block
	// The provider will emit the "request_completed" signal when done
	provider->send_request_with_messages(p_messages, context_block);
	
	// For async providers (OpenAI, Gemini, XAI), actions will be executed when signal fires
	// For DummyProvider, the signal fires immediately and actions execute synchronously
	
	return result_array;
}

// Provider management
void AI::set_provider(const Ref<AIProvider> &p_provider) {
    // Disconnect from old provider if it exists
    if (provider.is_valid()) {
        if (provider->is_connected("request_completed", callable_mp(this, &AI::_on_provider_request_completed))) {
            provider->disconnect("request_completed", callable_mp(this, &AI::_on_provider_request_completed));
    }
    }
    
    provider = p_provider;
    
    // Connect to new provider if it's valid
    if (provider.is_valid()) {
        provider->connect("request_completed", callable_mp(this, &AI::_on_provider_request_completed));
        print_line(vformat("AI: Provider set to %s", provider->get_class()));
    }
}

Ref<AIProvider> AI::get_provider() const {
    return provider;
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
    // Request with full conversation history
    ClassDB::bind_method(D_METHOD("request_actions_with_history", "messages"), &AI::request_actions_with_history);
    // Bind the helper method under the desired name for GDScript
    ClassDB::bind_method(D_METHOD("validate_command_json", "json_str"), &AI::_validate_command_json_bind);
    
    // Provider management
    ClassDB::bind_method(D_METHOD("set_provider", "provider"), &AI::set_provider);
    ClassDB::bind_method(D_METHOD("get_provider"), &AI::get_provider);
    
    // File operation helpers for UndoRedo
	ClassDB::bind_method(D_METHOD("_create_script_file", "abs_path", "content"), &AI::_create_script_file);
	ClassDB::bind_method(D_METHOD("_write_script_file", "abs_path", "content"), &AI::_write_script_file);
	ClassDB::bind_method(D_METHOD("_delete_script_file", "abs_path"), &AI::_delete_script_file);
	ClassDB::bind_method(D_METHOD("_rename_script_file", "old_abs_path", "new_abs_path"), &AI::_rename_script_file);
	ClassDB::bind_method(D_METHOD("_write_binary_file", "abs_path", "bytes"), &AI::_write_binary_file);
    
    // Add properties
    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "provider", PROPERTY_HINT_RESOURCE_TYPE, "AIProvider"), "set_provider", "get_provider");
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

// File operation helper implementations
void AI::_create_script_file(const String &abs_path, const String &content) {
    // Ensure parent directory exists
    String dir_path = abs_path.get_base_dir();
    if (!DirAccess::exists(dir_path)) {
        Error dir_err = DirAccess::make_dir_recursive_absolute(dir_path);
        if (dir_err != OK) {
            ERR_PRINT(vformat("AI: Failed to create directory '%s'. Error: %d", dir_path, dir_err));
            return;
        }
        print_verbose(vformat("AI: Created directory '%s'", dir_path));
    }

    Ref<GDScript> gd_script;
    gd_script.instantiate();
    gd_script->set_source_code(content);
    
    String res_path = ProjectSettings::get_singleton()->localize_path(abs_path);
    Error err = ResourceSaver::save(gd_script, res_path);
    if (err != OK) {
        ERR_PRINT(vformat("AI: Failed to create script at '%s'. Error: %d", abs_path, err));
        return;
    }
    print_verbose(vformat("AI: Created script at '%s'", abs_path));
}

void AI::_write_script_file(const String &abs_path, const String &content) {
    String res_path = ProjectSettings::get_singleton()->localize_path(abs_path);
    
    Ref<Script> gd_script = ResourceLoader::load(res_path);
    if (gd_script.is_null()) {
        ERR_PRINT(vformat("AI: Failed to load script at '%s' for writing.", abs_path));
        return;
    }
    
    gd_script->set_source_code(content);
    Error err = ResourceSaver::save(gd_script, res_path);
    if (err != OK) {
        ERR_PRINT(vformat("AI: Failed to save script at '%s'. Error: %d", abs_path, err));
        return;
    }
    print_verbose(vformat("AI: Updated script at '%s'", abs_path));
}

void AI::_delete_script_file(const String &abs_path) {
    Error err = DirAccess::remove_absolute(abs_path);
    if (err != OK) {
        ERR_PRINT(vformat("AI: Failed to delete file at '%s'. Error code: %d", abs_path, err));
        return;
    }
    print_verbose(vformat("AI: Deleted file at '%s'", abs_path));

    // Notify EditorFileSystem to refresh
    EditorFileSystem *efs = EditorFileSystem::get_singleton();
    if (efs) {
        efs->scan_changes();
    }
}

void AI::_rename_script_file(const String &old_abs_path, const String &new_abs_path) {
    // Ensure new_path directory exists
    String new_dir_path = new_abs_path.get_base_dir();
    if (!DirAccess::exists(new_dir_path)) {
        Error dir_err = DirAccess::make_dir_recursive_absolute(new_dir_path);
        if (dir_err != OK) {
            ERR_PRINT(vformat("AI: Failed to create directory '%s'. Error: %d", new_dir_path, dir_err));
            return;
        }
        print_verbose(vformat("AI: Created directory '%s'", new_dir_path));
    }

    // Use DirAccess to rename/move the file
    Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_RESOURCES);
    if (da.is_null()) {
        ERR_PRINT(vformat("AI: Failed to create DirAccess for renaming file."));
        return;
    }

    // Convert to resource paths for DirAccess
    String old_res_path = ProjectSettings::get_singleton()->localize_path(old_abs_path);
    String new_res_path = ProjectSettings::get_singleton()->localize_path(new_abs_path);

    Error err = da->rename(old_res_path, new_res_path);
    if (err != OK) {
        ERR_PRINT(vformat("AI: Failed to rename file from '%s' to '%s'. Error code: %d", old_abs_path, new_abs_path, err));
        return;
    }
    print_verbose(vformat("AI: Renamed file from '%s' to '%s'", old_abs_path, new_abs_path));

    // Notify EditorFileSystem to refresh
    EditorFileSystem *efs = EditorFileSystem::get_singleton();
    if (efs) {
        efs->scan_changes();
    }
}

void AI::_write_binary_file(const String &abs_path, const PackedByteArray &bytes) {
	Ref<FileAccess> file = FileAccess::open(abs_path, FileAccess::WRITE);
	if (file.is_null()) {
		ERR_PRINT(vformat("AI: Failed to open file for writing at '%s'.", abs_path));
		return;
	}
	file->store_buffer(bytes);
	file.unref();
	print_verbose(vformat("AI: Wrote binary file at '%s' (%d bytes)", abs_path, bytes.size()));
	// Notify EditorFileSystem about the change
	EditorFileSystem *efs = EditorFileSystem::get_singleton();
	if (efs) {
		efs->scan_changes();
	}
}

AI::AI() {
	// Set default provider to XAIProvider (Grok)
	Ref<XAIProvider> xai;
	xai.instantiate();
	set_provider(xai); // Use setter to connect signal

	// Initialize retrieval index
	retrieval.instantiate();

	// Set project root from ProjectSettings
	if (ProjectSettings::get_singleton()) {
		String project_root = ProjectSettings::get_singleton()->globalize_path("res://");
		retrieval->set_project_root(project_root);
		print_line(vformat("AI: Initialized retrieval index with project root: %s", project_root));
	}

	// Initialize agentic orchestrator
	orchestrator.instantiate();

	// Connect orchestrator signals to callbacks
	orchestrator->connect("progress_update", callable_mp(this, &AI::_on_agentic_progress));
	orchestrator->connect("tool_result_ready", callable_mp(this, &AI::_on_agentic_tool_result));
	orchestrator->connect("run_complete", callable_mp(this, &AI::_on_agentic_complete));

	print_line("AI: Agentic orchestrator initialized");
}

AI::~AI() {
    // Disconnect from provider if connected
    if (provider.is_valid()) {
        if (provider->is_connected("request_completed", callable_mp(this, &AI::_on_provider_request_completed))) {
            provider->disconnect("request_completed", callable_mp(this, &AI::_on_provider_request_completed));
        }
    }

    // Disconnect from orchestrator if connected
    if (orchestrator.is_valid()) {
        if (orchestrator->is_connected("progress_update", callable_mp(this, &AI::_on_agentic_progress))) {
            orchestrator->disconnect("progress_update", callable_mp(this, &AI::_on_agentic_progress));
        }
        if (orchestrator->is_connected("tool_result_ready", callable_mp(this, &AI::_on_agentic_tool_result))) {
            orchestrator->disconnect("tool_result_ready", callable_mp(this, &AI::_on_agentic_tool_result));
        }
        if (orchestrator->is_connected("run_complete", callable_mp(this, &AI::_on_agentic_complete))) {
            orchestrator->disconnect("run_complete", callable_mp(this, &AI::_on_agentic_complete));
        }
    }
}

// Agentic callback implementations
void AI::_on_agentic_tool_result(const Dictionary &p_tool_result) {
    // Tool result received from orchestrator
    // In Phase 4, we'll forward this to the UI for display in chat transcript
    print_verbose(vformat("AI: Tool result received: %s", JSON::stringify(p_tool_result)));
}

void AI::_on_agentic_progress(const String &p_status, int p_turn) {
    // Progress update from orchestrator
    // In Phase 4, we'll forward this to the UI status panel
    print_line(vformat("AI: Agentic progress (turn %d): %s", p_turn, p_status));
}

void AI::_on_agentic_complete(bool p_success, const String &p_final_message) {
    // Agentic run completed
    // In Phase 4, we'll update the UI with the final message
    if (p_success) {
        print_line(vformat("AI: Agentic run completed successfully: %s", p_final_message));
    } else {
        print_line(vformat("AI: Agentic run failed or was cancelled: %s", p_final_message));
    }
}

Ref<AgenticOrchestrator> AI::get_orchestrator() const {
    return orchestrator;
}
