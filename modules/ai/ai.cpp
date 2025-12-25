// modules/ai/ai.cpp
#include "ai.h"
#include "ai_provider.h"
#include "retrieval.h"

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
        // Check if parent_path matches the scene root's name
        if (parent_path_str == edited_scene_root->get_name()) {
            parent_node = edited_scene_root;
        } else {
            parent_node = edited_scene_root->get_node(NodePath(parent_path_str));
            if (!parent_node) {
                ERR_PRINT(vformat("AI Execute 'create_node': Could not find parent node at path '%s'. Using scene root instead.", parent_path_str));
                parent_node = edited_scene_root;
            }
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
    
    new_node->set_name(node_name);
    
    // Set up undo/redo - commit_action() will execute do_methods immediately
    undo_redo->create_action("AI Create Node");
    undo_redo->add_do_method(parent_node, "add_child", new_node, true); // force_readable_name = true
    undo_redo->add_do_method(new_node, "set_owner", edited_scene_root);
    undo_redo->add_do_reference(new_node); // Keep reference during undo/redo
    undo_redo->add_undo_method(parent_node, "remove_child", new_node);
    undo_redo->add_undo_method(new_node, "queue_free");
    undo_redo->commit_action(); // Executes do_methods immediately, making node findable
    
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

void AI::_execute_create_script(const Dictionary &args) {
    EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
    if (!undo_redo) {
        ERR_PRINT("AI Execute: EditorUndoRedoManager singleton not found.");
        return;
    }

    String file_path = args["file_path"];
    String language = args["language"];
    String content = args["content"];

    // Validate language (v0: GDScript only)
    if (language != "GDScript") {
        ERR_PRINT(vformat("AI Execute 'create_script': Only 'GDScript' language is supported in v0. Got: '%s'", language));
        return;
    }

    // Convert to absolute path if it's a resource path
    String abs_path = ProjectSettings::get_singleton()->globalize_path(file_path);

    // Check if file already exists
    if (FileAccess::exists(abs_path)) {
        WARN_PRINT(vformat("AI Execute 'create_script': File already exists at '%s'. Skipping creation.", abs_path));
        return;
    }

    print_verbose(vformat("AI: Creating script at '%s' with %d bytes of content", abs_path, content.length()));

    undo_redo->create_action("AI Create Script");
    undo_redo->add_do_method(this, "_create_script_file", abs_path, content);
    undo_redo->add_undo_method(this, "_delete_script_file", abs_path);
    undo_redo->commit_action();

    print_line(vformat("AI: Executed create_script. File: %s, Language: %s", file_path, language));
}

void AI::_execute_update_script(const Dictionary &args) {
    EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
    if (!undo_redo) {
        ERR_PRINT("AI Execute: EditorUndoRedoManager singleton not found.");
        return;
    }

    String file_path = args["file_path"];
    String patch_content = args["patch"];

    // Convert to absolute path if it's a resource path
    String abs_path = ProjectSettings::get_singleton()->globalize_path(file_path);

    // Check if file exists
    if (!FileAccess::exists(abs_path)) {
        ERR_PRINT(vformat("AI Execute 'update_script': File does not exist at '%s'. Cannot update.", abs_path));
        return;
    }

    // Read existing content for undo
    Ref<FileAccess> file = FileAccess::open(abs_path, FileAccess::READ);
    if (file.is_null()) {
        ERR_PRINT(vformat("AI Execute 'update_script': Failed to open file for reading at '%s'.", abs_path));
        return;
    }
    String original_content = file->get_as_text();
    file.unref(); // Close the file

    print_verbose(vformat("AI: Updating script at '%s'. Old size: %d bytes, New size: %d bytes", abs_path, original_content.length(), patch_content.length()));

    undo_redo->create_action("AI Update Script");
    undo_redo->add_do_method(this, "_write_script_file", abs_path, patch_content);
    undo_redo->add_undo_method(this, "_write_script_file", abs_path, original_content);
    undo_redo->commit_action();

    print_line(vformat("AI: Executed update_script. File: %s", file_path));
}

void AI::_execute_attach_script(const Dictionary &args) {
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
        ERR_PRINT("AI Execute 'attach_script': No edited scene root.");
        return;
    }

    String node_path_str = args["node_path"];
    String script_path = args["script_path"];

    // Resolve the target node
    Node *target_node = edited_scene_root->get_node(NodePath(node_path_str));
    if (!target_node) {
        ERR_PRINT(vformat("AI Execute 'attach_script': Could not find node at path '%s'.", node_path_str));
        return;
    }

    // Load script resource
    Ref<Script> scr = ResourceLoader::load(script_path);
    if (scr.is_null()) {
        ERR_PRINT(vformat("AI Execute 'attach_script': Could not load script at '%s'.", script_path));
        return;
    }

    // Save old script for undo (may be null)
    Variant old_script = target_node->get("script");

    // Wrap in UndoRedo
    undo_redo->create_action("AI Attach Script");
    undo_redo->add_do_method(target_node, "set", "script", scr);
    undo_redo->add_undo_method(target_node, "set", "script", old_script);
    undo_redo->commit_action();

    print_verbose(vformat("AIHelper: attach_script to node: %s", node_path_str));
    print_line(vformat("AI: Executed attach_script. Node: %s, Script: %s", node_path_str, script_path));
}

void AI::_on_provider_request_completed(bool success, const String &response_json, const String &error_message) {
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

    if (parsed_data.get_type() == Variant::ARRAY) {
        Array commands_array = parsed_data;
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
    } else {
        ERR_PRINT(vformat("AI::_process_and_execute_actions - AI response is not an Array. Got %s. Response: %s", Variant::get_type_name(parsed_data.get_type()), ai_json_response));
        return;
    }

    // Execute valid actions
    if (!valid_actions_array.is_empty()) {
        print_line(vformat("AI: Received %d valid actions. Executing...", valid_actions_array.size()));
        for (int i = 0; i < valid_actions_array.size(); ++i) {
            const Dictionary &action_dict = valid_actions_array[i];
            String action_name = action_dict.get("action", "");
            Variant args_variant = action_dict.get("args", Dictionary());

            if (args_variant.get_type() == Variant::DICTIONARY) {
                Dictionary action_args = args_variant;
                print_line(vformat("  - Executing Action %d: %s", i + 1, JSON::stringify(action_dict)));
                if (action_name == "create_node") {
                    _execute_create_node(action_args);
                } else if (action_name == "set_property") {
                    _execute_set_property(action_args);
                } else if (action_name == "create_script") {
                    _execute_create_script(action_args);
                } else if (action_name == "update_script") {
                    _execute_update_script(action_args);
                } else if (action_name == "attach_script") {
                    _execute_attach_script(action_args);
                } else {
                    print_line(vformat("    - Action '%s' has no execution logic implemented.", action_name));
                }
            } else {
                WARN_PRINT(vformat("  - Action %d ('%s') has invalid 'args' type. Skipping.", i+1, action_name));
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
    // Bind the helper method under the desired name for GDScript
    ClassDB::bind_method(D_METHOD("validate_command_json", "json_str"), &AI::_validate_command_json_bind);
    
    // Provider management
    ClassDB::bind_method(D_METHOD("set_provider", "provider"), &AI::set_provider);
    ClassDB::bind_method(D_METHOD("get_provider"), &AI::get_provider);
    
    // File operation helpers for UndoRedo
    ClassDB::bind_method(D_METHOD("_create_script_file", "abs_path", "content"), &AI::_create_script_file);
    ClassDB::bind_method(D_METHOD("_write_script_file", "abs_path", "content"), &AI::_write_script_file);
    ClassDB::bind_method(D_METHOD("_delete_script_file", "abs_path"), &AI::_delete_script_file);
    
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
}

AI::~AI() {
    // Disconnect from provider if connected
    if (provider.is_valid()) {
        if (provider->is_connected("request_completed", callable_mp(this, &AI::_on_provider_request_completed))) {
            provider->disconnect("request_completed", callable_mp(this, &AI::_on_provider_request_completed));
        }
    }
}
