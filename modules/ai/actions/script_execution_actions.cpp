// modules/ai/actions/script_execution_actions.cpp
// run_editor_script: compile agent-authored GDScript from a string in memory
// and execute it inside the editor process, against the live engine API.
// The script never touches res:// or disk and is invisible to the FileSystem
// dock. Runs on the editor main thread — a runaway script freezes the editor
// (accepted v1 trade, see docs/design/script_execution_plan.md §4/§8).

#include "script_execution_actions.h"
#include "action_common.h"

#include "core/object/class_db.h"
#include "core/object/ref_counted.h"
#include "core/object/script_language.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/variant/variant_parser.h"
#include "modules/gdscript/gdscript.h"
#include "modules/gdscript/gdscript_analyzer.h"
#include "modules/gdscript/gdscript_parser.h"

namespace {

#ifdef TOOLS_ENABLED

// Captures engine/script errors raised while the agent's script compiles and
// runs — the add_error_handler pattern from AISceneLoadErrorCapture
// (scene_actions.cpp), generalized to structured entries. Handler invocations
// are serialized by the global error-handler lock, and entries are only read
// after remove_error_handler, so no extra locking is needed.
struct AIScriptRunErrorCapture {
	struct Entry {
		String function;
		String file;
		int line = 0;
		String message;
		bool is_script_error = false; // ERR_HANDLER_SCRIPT (GDScript runtime / push_error) vs core engine error.
	};
	Vector<Entry> entries;
};

static void _ai_script_run_error_handler(void *p_userdata, const char *p_function, const char *p_file, int p_line, const char *p_error, const char *p_message, bool p_editor_notify, ErrorHandlerType p_type) {
	if (p_type != ERR_HANDLER_ERROR && p_type != ERR_HANDLER_SCRIPT) {
		return;
	}
	AIScriptRunErrorCapture *capture = static_cast<AIScriptRunErrorCapture *>(p_userdata);
	AIScriptRunErrorCapture::Entry e;
	e.function = String::utf8(p_function);
	e.file = String::utf8(p_file);
	e.line = p_line;
	String msg = String::utf8(p_error);
	String explanation = String::utf8(p_message);
	if (!explanation.is_empty() && explanation != msg) {
		msg += " (" + explanation + ")";
	}
	e.message = msg;
	e.is_script_error = p_type == ERR_HANDLER_SCRIPT;
	capture->entries.push_back(e);
}

// Captures everything printed while the script runs (print/print_rich/printerr
// all flow through the global print handlers). Same locking story as above.
struct AIScriptRunPrintCapture {
	Vector<String> lines;
};

static void _ai_script_run_print_handler(void *p_userdata, const String &p_string, bool p_error, bool p_rich) {
	AIScriptRunPrintCapture *capture = static_cast<AIScriptRunPrintCapture *>(p_userdata);
	capture->lines.push_back(p_string);
}

// p_script_path: the synthetic "gdscript://<id>.gd" path the GDScript
// constructor assigns to pathless in-memory scripts — errors reported against
// it are the agent's own script, renamed so the model recognizes them. The
// built-in spellings are kept as a fallback for paths the VM substitutes.
static Dictionary _capture_entry_to_dict(const AIScriptRunErrorCapture::Entry &p_entry, const String &p_script_path) {
	Dictionary d;
	bool is_own_script = (!p_script_path.is_empty() && p_entry.file == p_script_path) ||
			p_entry.file == "<built-in>" || p_entry.file == "built-in";
	d["file"] = is_own_script ? String("<your script>") : p_entry.file;
	d["line"] = p_entry.line;
	d["function"] = p_entry.function;
	d["message"] = p_entry.message;
	d["type"] = p_entry.is_script_error ? "script" : "engine";
	return d;
}

// Converts the script's return Variant into a JSON-friendly shape. Objects
// become short descriptors (never serialized wholesale), oversized packed
// arrays are summarized, and engine math types use their GDScript literal
// form (e.g. "Vector2(1, 2)") via VariantWriter.
static Variant _return_value_to_jsonable(const Variant &p_value, int p_depth) {
	if (p_depth > 16) {
		return String("<nesting too deep>");
	}
	switch (p_value.get_type()) {
		case Variant::NIL:
		case Variant::BOOL:
		case Variant::INT:
		case Variant::FLOAT:
		case Variant::STRING:
			return p_value;
		case Variant::STRING_NAME:
		case Variant::NODE_PATH:
			return String(p_value);
		case Variant::DICTIONARY: {
			Dictionary in = p_value;
			Dictionary out;
			Array keys = in.keys();
			for (int i = 0; i < keys.size(); i++) {
				const Variant &key = keys[i];
				out[String(key)] = _return_value_to_jsonable(in[key], p_depth + 1);
			}
			return out;
		}
		case Variant::ARRAY: {
			Array in = p_value;
			Array out;
			for (int i = 0; i < in.size(); i++) {
				out.push_back(_return_value_to_jsonable(in[i], p_depth + 1));
			}
			return out;
		}
		case Variant::OBJECT: {
			Object *obj = p_value.get_validated_object();
			if (!obj) {
				return String("<freed or invalid object>");
			}
			Node *node = Object::cast_to<Node>(obj);
			if (node) {
				return vformat("<Node %s (%s)>", node->is_inside_tree() ? String(node->get_path()) : String(node->get_name()), node->get_class());
			}
			Resource *res = Object::cast_to<Resource>(obj);
			if (res) {
				return vformat("<Resource %s (%s)>", res->get_path().is_empty() ? String("embedded") : res->get_path(), res->get_class());
			}
			return vformat("<Object %s>", obj->get_class());
		}
#define AI_PACKED_ARRAY_CASE(m_variant_type, m_ctype)                                                       \
	case Variant::m_variant_type: {                                                                         \
		m_ctype arr = p_value;                                                                              \
		if (arr.size() > 256) {                                                                             \
			return vformat("<%s size=%d>", Variant::get_type_name(p_value.get_type()), arr.size());         \
		}                                                                                                   \
		Array out;                                                                                          \
		for (int i = 0; i < arr.size(); i++) {                                                              \
			out.push_back(_return_value_to_jsonable(arr[i], p_depth + 1));                                  \
		}                                                                                                   \
		return out;                                                                                         \
	}
			AI_PACKED_ARRAY_CASE(PACKED_BYTE_ARRAY, PackedByteArray)
			AI_PACKED_ARRAY_CASE(PACKED_INT32_ARRAY, PackedInt32Array)
			AI_PACKED_ARRAY_CASE(PACKED_INT64_ARRAY, PackedInt64Array)
			AI_PACKED_ARRAY_CASE(PACKED_FLOAT32_ARRAY, PackedFloat32Array)
			AI_PACKED_ARRAY_CASE(PACKED_FLOAT64_ARRAY, PackedFloat64Array)
			AI_PACKED_ARRAY_CASE(PACKED_STRING_ARRAY, PackedStringArray)
			AI_PACKED_ARRAY_CASE(PACKED_VECTOR2_ARRAY, PackedVector2Array)
			AI_PACKED_ARRAY_CASE(PACKED_VECTOR3_ARRAY, PackedVector3Array)
			AI_PACKED_ARRAY_CASE(PACKED_COLOR_ARRAY, PackedColorArray)
			AI_PACKED_ARRAY_CASE(PACKED_VECTOR4_ARRAY, PackedVector4Array)
#undef AI_PACKED_ARRAY_CASE
		default:
			break;
	}
	// Math types, Callable, Signal, RID... — GDScript literal form reads best.
	String repr;
	if (VariantWriter::write_to_string(p_value, repr) == OK) {
		return repr;
	}
	return String(p_value);
}

#endif // TOOLS_ENABLED

} // namespace

namespace AIScriptExecActions {

Dictionary exec_run_editor_script(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	if (!args.has("script") || args["script"].get_type() != Variant::STRING) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"'script' must be a string containing full GDScript source");
	}
	String source = args["script"];
	if (source.strip_edges().is_empty()) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"'script' is empty — provide full GDScript source defining `func run() -> Variant`.");
	}

	// Phase 1: parse + analyze the source exactly as provided, so error line
	// numbers match the model's own text (same pipeline as create_script).
	bool declares_tool = false;
	{
		GDScriptParser parser;
		Error parse_err = parser.parse(source, String(), false);

		Array parse_errors;
		for (const GDScriptParser::ParserError &e : parser.get_errors()) {
			Dictionary err_dict;
			err_dict["line"] = e.line;
			err_dict["column"] = e.column;
			err_dict["message"] = e.message;
			err_dict["type"] = "syntax";
			parse_errors.push_back(err_dict);
		}
		if (parse_err == OK && parser.get_errors().is_empty()) {
			GDScriptAnalyzer analyzer(&parser);
			analyzer.analyze();
			for (const GDScriptParser::ParserError &e : parser.get_errors()) {
				Dictionary err_dict;
				err_dict["line"] = e.line;
				err_dict["column"] = e.column;
				err_dict["message"] = e.message;
				err_dict["type"] = "semantic";
				parse_errors.push_back(err_dict);
			}
		}
		if (!parse_errors.is_empty()) {
			Dictionary details;
			details["parse_errors"] = parse_errors;
			return ai_create_error_result("parse_error",
				vformat("Script failed to compile with %d error(s). Nothing was executed. See 'parse_errors' in details — line numbers refer to your script source.", parse_errors.size()),
				details);
		}
		declares_tool = parser.is_tool();
	}

	// The editor process runs with scripting disabled for non-tool scripts
	// (EditorNode calls ScriptServer::set_scripting_enabled(false)) — without
	// @tool the instance would be a placeholder and run() would never execute.
	// Requiring the annotation (rather than injecting it) keeps reported error
	// line numbers 1:1 with the source the model sent.
	if (!declares_tool) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"The script must declare `@tool` as its first line — the editor process only runs tool scripts. Resend the full script starting with `@tool`.");
	}

	AIScriptRunErrorCapture error_capture;
	ErrorHandlerList error_handler;
	error_handler.errfunc = _ai_script_run_error_handler;
	error_handler.userdata = &error_capture;

	AIScriptRunPrintCapture print_capture;
	PrintHandlerList print_handler;
	print_handler.printfunc = _ai_script_run_print_handler;
	print_handler.userdata = &print_capture;

	Ref<GDScript> gdscript;
	gdscript.instantiate();
	gdscript->set_source_code(source);
	// Pathless in-memory scripts get a synthetic "gdscript://<id>.gd" path in
	// the GDScript constructor — errors reported against it are ours.
	const String script_path = gdscript->get_script_path();

	add_error_handler(&error_handler);

	Error compile_err = gdscript->reload();
	if (compile_err != OK || !gdscript->is_valid()) {
		remove_error_handler(&error_handler);
		Array errors;
		for (const AIScriptRunErrorCapture::Entry &e : error_capture.entries) {
			errors.push_back(_capture_entry_to_dict(e, script_path));
		}
		Dictionary details;
		details["errors"] = errors;
		return ai_create_error_result("parse_error",
			vformat("Script failed to compile (error %d). Nothing was executed. See 'errors' in details — line numbers refer to your script source.", compile_err),
			details);
	}

	if (!gdscript->has_method(StringName("run"))) {
		remove_error_handler(&error_handler);
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"The script must define `func run() -> Variant` at the top level — that is the entry point run_editor_script calls.");
	}

	StringName base_type = gdscript->get_instance_base_type();
	Object *instance = base_type == StringName() ? nullptr : ClassDB::instantiate(base_type);
	if (!instance) {
		remove_error_handler(&error_handler);
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("Could not instantiate the script's base type '%s'. Extend an instantiable class (e.g. RefCounted, Node, Node2D) or omit `extends` for RefCounted.",
				base_type == StringName() ? String("<unknown>") : String(base_type)));
	}

	// A RefCounted-based instance is owned via Ref (released on return); any
	// other Object is memdeleted below — unless the script freed it itself or
	// parented it into a scene tree, hence the ObjectID/is_inside_tree guards.
	const ObjectID instance_id = instance->get_instance_id();
	RefCounted *instance_rc = Object::cast_to<RefCounted>(instance);
	Ref<RefCounted> instance_ref;
	if (instance_rc) {
		instance_ref = Ref<RefCounted>(instance_rc);
	}

	// From here the script's own code runs (_init during set_script, then run()).
	add_print_handler(&print_handler);
	const uint64_t start_usec = OS::get_singleton()->get_ticks_usec();

	instance->set_script(gdscript);

	ScriptInstance *script_instance = instance->get_script_instance();
	if (!script_instance || script_instance->is_placeholder()) {
		remove_print_handler(&print_handler);
		remove_error_handler(&error_handler);
		if (!instance_rc && ObjectDB::get_instance(instance_id)) {
			memdelete(instance);
		}
		Array errors;
		for (const AIScriptRunErrorCapture::Entry &e : error_capture.entries) {
			errors.push_back(_capture_entry_to_dict(e, script_path));
		}
		Dictionary details;
		details["errors"] = errors;
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			"The script compiled but could not produce a runnable instance (does _init() have required parameters?). See 'errors' in details.", details);
	}

	Callable::CallError call_error;
	Variant return_value = instance->callp(StringName("run"), nullptr, 0, call_error);

	const uint64_t elapsed_usec = OS::get_singleton()->get_ticks_usec() - start_usec;
	remove_print_handler(&print_handler);
	remove_error_handler(&error_handler);

	if (!instance_rc) {
		Object *alive = ObjectDB::get_instance(instance_id);
		Node *alive_node = Object::cast_to<Node>(alive);
		if (alive && !(alive_node && alive_node->is_inside_tree())) {
			memdelete(alive);
		}
	}
	instance = nullptr;

	Array prints;
	for (const String &line : print_capture.lines) {
		prints.push_back(line);
	}
	Array errors;
	bool script_error_raised = false;
	for (const AIScriptRunErrorCapture::Entry &e : error_capture.entries) {
		errors.push_back(_capture_entry_to_dict(e, script_path));
		if (e.is_script_error) {
			script_error_raised = true;
		}
	}
	const double execution_time_ms = double(elapsed_usec) / 1000.0;

	if (call_error.error != Callable::CallError::CALL_OK) {
		Dictionary details;
		details["errors"] = errors;
		if (!prints.is_empty()) {
			details["prints"] = prints;
		}
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			"run() could not be called — it must be callable with no arguments (`func run() -> Variant`).", details);
	}

	// A GDScript runtime error aborts the whole call chain and surfaces here as
	// a captured script error plus a nil return — report that as a failure. (A
	// script that raised errors but still returned a value gets a success with
	// the errors listed; a nil return alongside its own push_error is the one
	// ambiguous case, and it reads as a failure, which is the safe direction.)
	if (return_value.get_type() == Variant::NIL && script_error_raised) {
		Dictionary details;
		details["errors"] = errors;
		if (!prints.is_empty()) {
			details["prints"] = prints;
		}
		details["execution_time_ms"] = execution_time_ms;
		return ai_create_error_result("runtime_error",
			"The script raised runtime error(s) and run() produced no return value — it most likely aborted at the first error in 'errors'. Line numbers refer to your script source.", details);
	}

	Dictionary result_data;
	result_data["return_value"] = _return_value_to_jsonable(return_value, 0);
	result_data["return_type"] = Variant::get_type_name(return_value.get_type());
	result_data["prints"] = prints;
	result_data["execution_time_ms"] = execution_time_ms;
	if (!errors.is_empty()) {
		result_data["errors"] = errors;
		result_data["note"] = "Engine errors were raised during execution (see 'errors') — verify the script actually did what you intended.";
	}

	print_line(vformat("AI: Executed run_editor_script (%d chars, %.1f ms, %d print(s), %d error(s))",
		source.length(), execution_time_ms, prints.size(), errors.size()));
	return ai_create_success_result(result_data);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

} // namespace AIScriptExecActions
