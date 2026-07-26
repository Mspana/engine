// modules/ai/actions/export_actions.cpp
// Export-related action implementations for the AI module

#include "export_actions.h"
#include "action_common.h"

#include "core/config/project_settings.h"
#include "core/error/error_list.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/version.h"
#include "editor/editor_node.h"
#include "editor/editor_paths.h"
#include "editor/editor_settings.h"
#include "editor/export/editor_export.h"

namespace AIExportActions {

// ==========================================================================
// Helpers
// ==========================================================================

static String _export_filter_to_string(EditorExportPreset::ExportFilter p_filter) {
	switch (p_filter) {
		case EditorExportPreset::EXPORT_ALL_RESOURCES:
			return "all_resources";
		case EditorExportPreset::EXPORT_SELECTED_SCENES:
			return "scenes";
		case EditorExportPreset::EXPORT_SELECTED_RESOURCES:
			return "resources";
		case EditorExportPreset::EXCLUDE_SELECTED_RESOURCES:
			return "exclude";
		case EditorExportPreset::EXPORT_CUSTOMIZED:
			return "customized";
	}
	return "all_resources";
}

// Returns true and fills r_filter on success.
static bool _export_filter_from_string(const String &p_str, EditorExportPreset::ExportFilter &r_filter) {
	if (p_str == "all_resources") {
		r_filter = EditorExportPreset::EXPORT_ALL_RESOURCES;
	} else if (p_str == "scenes") {
		r_filter = EditorExportPreset::EXPORT_SELECTED_SCENES;
	} else if (p_str == "resources") {
		r_filter = EditorExportPreset::EXPORT_SELECTED_RESOURCES;
	} else if (p_str == "exclude") {
		r_filter = EditorExportPreset::EXCLUDE_SELECTED_RESOURCES;
	} else if (p_str == "customized") {
		r_filter = EditorExportPreset::EXPORT_CUSTOMIZED;
	} else {
		return false;
	}
	return true;
}

static Ref<EditorExportPlatform> _find_platform(const String &p_name, Dictionary &r_error) {
	EditorExport *ee = EditorExport::get_singleton();
	if (!ee) {
		r_error = ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			"EditorExport singleton not available");
		return Ref<EditorExportPlatform>();
	}
	PackedStringArray available;
	for (int i = 0; i < ee->get_export_platform_count(); i++) {
		Ref<EditorExportPlatform> platform = ee->get_export_platform(i);
		if (platform.is_null()) {
			continue;
		}
		if (platform->get_name().nocasecmp_to(p_name) == 0) {
			return platform;
		}
		available.push_back(platform->get_name());
	}
	Dictionary details;
	details["available_platforms"] = available;
	r_error = ai_create_error_result(AIErrorCodes::INVALID_ARGS,
		vformat("Unknown export platform '%s'.", p_name), details);
	return Ref<EditorExportPlatform>();
}

static Ref<EditorExportPreset> _find_preset(const String &p_name, Dictionary &r_error) {
	EditorExport *ee = EditorExport::get_singleton();
	if (!ee) {
		r_error = ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			"EditorExport singleton not available");
		return Ref<EditorExportPreset>();
	}
	// Exact match first, then case-insensitive.
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < ee->get_export_preset_count(); i++) {
			Ref<EditorExportPreset> preset = ee->get_export_preset(i);
			if (preset.is_null()) {
				continue;
			}
			bool match = (pass == 0) ? (preset->get_name() == p_name)
									 : (preset->get_name().nocasecmp_to(p_name) == 0);
			if (match) {
				return preset;
			}
		}
	}
	PackedStringArray existing;
	for (int i = 0; i < ee->get_export_preset_count(); i++) {
		Ref<EditorExportPreset> preset = ee->get_export_preset(i);
		if (preset.is_valid()) {
			existing.push_back(preset->get_name());
		}
	}
	Dictionary details;
	details["existing_presets"] = existing;
	if (existing.is_empty()) {
		details["hint"] = "No export presets exist yet — create one with create_export_preset.";
	}
	r_error = ai_create_error_result(AIErrorCodes::INVALID_ARGS,
		vformat("No export preset named '%s'.", p_name), details);
	return Ref<EditorExportPreset>();
}

static Dictionary _preset_summary(const Ref<EditorExportPreset> &p_preset) {
	Dictionary d;
	d["name"] = p_preset->get_name();
	Ref<EditorExportPlatform> platform = p_preset->get_platform();
	d["platform"] = platform.is_valid() ? platform->get_name() : String("unknown");
	d["runnable"] = p_preset->is_runnable();
	d["export_path"] = p_preset->get_export_path();
	d["export_filter"] = _export_filter_to_string(p_preset->get_export_filter());
	d["export_files_count"] = p_preset->get_files_to_export().size();
	d["include_filter"] = p_preset->get_include_filter();
	d["exclude_filter"] = p_preset->get_exclude_filter();
	return d;
}

static Vector<String> _filter_entries(const String &p_filter) {
	Vector<String> entries;
	for (const String &raw : p_filter.split(",")) {
		String entry = raw.strip_edges();
		if (!entry.is_empty()) {
			entries.push_back(entry);
		}
	}
	return entries;
}

// Recursive res:// walk collecting project-relative file paths ("dir/file.ext",
// no res:// prefix). Skips dot-directories and directories containing .gdignore,
// mirroring what the export file scan skips.
static void _collect_project_files_recursive(const String &p_dir_rel, Vector<String> &r_files) {
	String dir_res = p_dir_rel.is_empty() ? String("res://") : "res://" + p_dir_rel;
	Ref<DirAccess> da = DirAccess::open(dir_res);
	if (da.is_null()) {
		return;
	}
	da->list_dir_begin();
	String name = da->get_next();
	while (!name.is_empty()) {
		String rel = p_dir_rel.is_empty() ? name : p_dir_rel + "/" + name;
		if (da->current_is_dir()) {
			if (!name.begins_with(".") && !FileAccess::exists("res://" + rel + "/.gdignore")) {
				_collect_project_files_recursive(rel, r_files);
			}
		} else {
			r_files.push_back(rel);
		}
		name = da->get_next();
	}
	da->list_dir_end();
}

// The two silent-misconfiguration checks can_export() does not perform.
// Both were observed producing a near-empty pck with zero engine warnings.
static Array _preset_misconfig_warnings(const Ref<EditorExportPreset> &p_preset) {
	Array warnings;

	EditorExportPreset::ExportFilter filter = p_preset->get_export_filter();
	bool selective = filter == EditorExportPreset::EXPORT_SELECTED_SCENES ||
			filter == EditorExportPreset::EXPORT_SELECTED_RESOURCES;
	if (selective && p_preset->get_files_to_export().is_empty()) {
		warnings.push_back(vformat(
			"export_filter is '%s' but export_files is EMPTY — this exports only autoloads and their preload chains (a near-empty pck) with no engine error. Set export_files, or set export_filter to 'all_resources'.",
			_export_filter_to_string(filter)));
	}

	Vector<String> include_entries = _filter_entries(p_preset->get_include_filter());
	Vector<String> exclude_entries = _filter_entries(p_preset->get_exclude_filter());
	if (!include_entries.is_empty() || !exclude_entries.is_empty()) {
		Vector<String> project_files;
		_collect_project_files_recursive(String(), project_files);
		for (int pass = 0; pass < 2; pass++) {
			const Vector<String> &entries = (pass == 0) ? include_entries : exclude_entries;
			const char *which = (pass == 0) ? "include_filter" : "exclude_filter";
			for (const String &entry : entries) {
				bool matched = false;
				for (const String &rel : project_files) {
					// Same dual-form case-insensitive glob the engine applies
					// during export (_edit_files_with_filter).
					if (("res://" + rel).matchn(entry) || rel.matchn(entry)) {
						matched = true;
						break;
					}
				}
				if (!matched) {
					warnings.push_back(vformat(
						"%s entry '%s' matches ZERO project files. Filters are comma-separated case-insensitive globs matched against both 'res://dir/file' and 'dir/file' (e.g. '*.txt' or 'assets/*.json') — a bare res:// path with no wildcard usually means a misunderstanding.",
						which, entry));
				}
			}
		}
	}

	return warnings;
}

// Reads the embedded file count from a standalone .pck header.
// Layout (little-endian): u32 magic "GDPC" @0, u32 format_version @4,
// 3×u32 engine version @8..19, u32 flags @20, u64 file_base @24,
// 16×u32 reserved @32..95, u32 file_count @96.
// Returns -1 if the file is missing or not a v2 pck.
static int64_t _read_pck_file_count(const String &p_abs_path) {
	Ref<FileAccess> f = FileAccess::open(p_abs_path, FileAccess::READ);
	if (f.is_null()) {
		return -1;
	}
	if (f->get_length() < 100) {
		return -1;
	}
	uint32_t magic = f->get_32();
	if (magic != 0x43504447) { // "GDPC"
		return -1;
	}
	uint32_t format_version = f->get_32();
	if (format_version != 2) {
		return -1;
	}
	f->seek(96);
	return (int64_t)f->get_32();
}

String get_templates_dir() {
	EditorPaths *paths = EditorPaths::get_singleton();
	if (!paths) {
		return String();
	}
	return paths->get_export_templates_dir().path_join(GODOT_VERSION_FULL_CONFIG);
}

static int _count_files_recursive(const String &p_abs_dir) {
	Ref<DirAccess> da = DirAccess::open(p_abs_dir);
	if (da.is_null()) {
		return 0;
	}
	int count = 0;
	da->list_dir_begin();
	String name = da->get_next();
	while (!name.is_empty()) {
		if (da->current_is_dir()) {
			if (name != "." && name != "..") {
				count += _count_files_recursive(p_abs_dir.path_join(name));
			}
		} else {
			count++;
		}
		name = da->get_next();
	}
	da->list_dir_end();
	return count;
}

int count_template_files() {
	String dir = get_templates_dir();
	if (dir.is_empty()) {
		return 0;
	}
	return _count_files_recursive(dir);
}

bool templates_installed() {
	String dir = get_templates_dir();
	return !dir.is_empty() && DirAccess::exists(dir) && count_template_files() > 0;
}

// Resolves a preset/tool output path to an absolute OS path. Preset export
// paths are stored project-relative and may climb out of the project
// (e.g. "../builds/index.html").
static String _resolve_output_path(const String &p_path) {
	if (p_path.begins_with("res://")) {
		return ProjectSettings::get_singleton()->globalize_path(p_path);
	}
	if (p_path.is_absolute_path()) {
		return p_path;
	}
	String project_root = ProjectSettings::get_singleton()->globalize_path("res://");
	return project_root.path_join(p_path).simplify_path();
}

static String _error_to_string(Error p_err) {
	return vformat("%s (%d)", error_names[p_err], (int)p_err);
}

static const char *EXPORT_FILTER_VALUES = "all_resources, scenes, resources, exclude, customized";

// ==========================================================================
// Tier 1 — read and configure
// ==========================================================================

Dictionary exec_get_export_status(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	EditorExport *ee = EditorExport::get_singleton();
	if (!ee) {
		return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			"EditorExport singleton not available");
	}

	String platform_filter = args.get("platform", String());

	Dictionary result_data;
	result_data["engine_version"] = GODOT_VERSION_FULL_CONFIG;

	// Template installation state.
	Dictionary templates;
	String tpl_dir = get_templates_dir();
	templates["dir"] = tpl_dir;
	bool tpl_installed = templates_installed();
	templates["installed"] = tpl_installed;
	if (tpl_installed) {
		templates["file_count"] = count_template_files();
	}
	result_data["templates"] = templates;

	// Renderer info — the classic web-export trap. Note the .web feature
	// override defaults to gl_compatibility even when the main renderer is
	// forward_plus, so web exports usually work without switching.
	ProjectSettings *ps = ProjectSettings::get_singleton();
	Dictionary renderer;
	renderer["rendering_method"] = ps->get_setting("rendering/renderer/rendering_method", "forward_plus");
	renderer["rendering_method_web"] = ps->get_setting("rendering/renderer/rendering_method.web", "gl_compatibility");
	result_data["renderer"] = renderer;

	// Platforms.
	PackedStringArray platform_names;
	for (int i = 0; i < ee->get_export_platform_count(); i++) {
		Ref<EditorExportPlatform> platform = ee->get_export_platform(i);
		if (platform.is_valid()) {
			platform_names.push_back(platform->get_name());
		}
	}
	result_data["platforms"] = platform_names;

	// Presets with can-export verdicts and misconfiguration warnings.
	Array presets;
	bool any_missing_templates = false;
	for (int i = 0; i < ee->get_export_preset_count(); i++) {
		Ref<EditorExportPreset> preset = ee->get_export_preset(i);
		if (preset.is_null()) {
			continue;
		}
		Ref<EditorExportPlatform> platform = preset->get_platform();
		if (!platform_filter.is_empty() && platform.is_valid() &&
				platform->get_name().nocasecmp_to(platform_filter) != 0) {
			continue;
		}
		Dictionary entry = _preset_summary(preset);
		if (platform.is_valid()) {
			String export_error;
			bool missing_templates = false;
			bool ok = platform->can_export(preset, export_error, missing_templates, /*p_debug=*/false);
			entry["can_export"] = ok;
			entry["missing_templates"] = missing_templates;
			any_missing_templates = any_missing_templates || missing_templates;
			if (!export_error.strip_edges().is_empty()) {
				entry["export_errors"] = export_error.strip_edges().split("\n");
			}
			// Debug verdict can differ (separate debug templates) — only
			// report it when it does. Serving web builds uses debug.
			String debug_error;
			bool debug_missing = false;
			bool debug_ok = platform->can_export(preset, debug_error, debug_missing, /*p_debug=*/true);
			if (debug_ok != ok) {
				entry["can_export_debug"] = debug_ok;
				if (!debug_error.strip_edges().is_empty()) {
					entry["debug_export_errors"] = debug_error.strip_edges().split("\n");
				}
			}
		}
		entry["warnings"] = _preset_misconfig_warnings(preset);
		presets.push_back(entry);
	}
	result_data["presets"] = presets;

	// Workflow hint for the model.
	if (presets.is_empty()) {
		result_data["workflow_hint"] = "No export presets exist. Create one with create_export_preset (platform 'Web' for browser builds), then export_project. Web builds can be tested locally with serve_web_build.";
	} else if (!tpl_installed || any_missing_templates) {
		result_data["workflow_hint"] = "Export templates for this engine version are missing. Install them with install_export_templates (one-time, large download), then export_project.";
	}

	return ai_create_success_result(result_data);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

Dictionary exec_create_export_preset(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	if (!args.has("platform") || args["platform"].get_type() != Variant::STRING) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"'platform' must be a string (e.g. 'Web')");
	}

	Dictionary error;
	Ref<EditorExportPlatform> platform = _find_platform(args["platform"], error);
	if (platform.is_null()) {
		return error;
	}

	String name = args.get("name", String());
	if (name.strip_edges().is_empty()) {
		name = platform->get_name();
	}
	name = name.strip_edges();

	EditorExport *ee = EditorExport::get_singleton();
	for (int i = 0; i < ee->get_export_preset_count(); i++) {
		Ref<EditorExportPreset> existing = ee->get_export_preset(i);
		if (existing.is_valid() && existing->get_name() == name) {
			Dictionary details;
			details["hint"] = "Use set_export_preset_option to modify the existing preset, or pass a different name.";
			return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
				vformat("An export preset named '%s' already exists.", name), details);
		}
	}

	EditorExportPreset::ExportFilter export_filter = EditorExportPreset::EXPORT_ALL_RESOURCES;
	if (args.has("export_filter")) {
		if (!_export_filter_from_string(args["export_filter"], export_filter)) {
			return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
				vformat("Invalid export_filter '%s'. Valid values: %s.", args["export_filter"], EXPORT_FILTER_VALUES));
		}
	}

	// create_preset() fills every platform option with its default value.
	Ref<EditorExportPreset> preset = platform->create_preset();
	if (preset.is_null()) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("Platform '%s' could not create a preset.", platform->get_name()));
	}

	String export_path = args.get("export_path", String());
	if (!export_path.is_empty()) {
		String ext = export_path.get_extension().to_lower();
		List<String> valid_exts = platform->get_binary_extensions(preset);
		bool ext_ok = false;
		PackedStringArray ext_list;
		for (const String &e : valid_exts) {
			ext_list.push_back(e);
			if (ext == e.to_lower()) {
				ext_ok = true;
			}
		}
		if (!ext_ok) {
			Dictionary details;
			details["valid_extensions"] = ext_list;
			return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
				vformat("export_path extension '.%s' is not valid for platform '%s'.", ext, platform->get_name()),
				details);
		}
	}

	preset->set_name(name);
	if (export_filter != EditorExportPreset::EXPORT_ALL_RESOURCES) {
		preset->set_export_filter(export_filter);
	}
	if (!export_path.is_empty()) {
		preset->set_export_path(export_path);
	}
	ee->add_export_preset(preset);
	// Set runnable LAST: add_export_preset() does not arm the debounced save,
	// but every preset setter does — calling one after registration guarantees
	// the new preset is persisted (and emits the runnable-changed signal the
	// remote-deploy UI listens to).
	preset->set_runnable(args.get("runnable", true));

	ai_log_verbose(vformat("Created export preset '%s' for platform '%s'", name, platform->get_name()));

	Dictionary result_data = _preset_summary(preset);
	result_data["options_count"] = (int)preset->get_properties().size();
	Array notes;
	notes.push_back("res://export_presets.cfg will be rewritten (debounced ~0.8s). Not undoable.");
	notes.push_back("If the Project > Export dialog is currently open, reopen it to see the new preset.");
	if (platform->get_name() == "Web") {
		notes.push_back("Key Web defaults: variant/thread_support=false, vram_texture_compression/for_desktop=true, PWA disabled. Change with set_export_preset_option if needed.");
	}
	result_data["notes"] = notes;

	print_line(vformat("AI: Executed create_export_preset. Name: %s", name));
	return ai_create_success_result(result_data);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

Dictionary exec_set_export_preset_option(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	if (!args.has("preset") || args["preset"].get_type() != Variant::STRING) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"'preset' must be a string (the preset name)");
	}
	if (!args.has("option") || args["option"].get_type() != Variant::STRING) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"'option' must be a string");
	}
	if (!args.has("value")) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"'value' is required");
	}

	Dictionary error;
	Ref<EditorExportPreset> preset = _find_preset(args["preset"], error);
	if (preset.is_null()) {
		return error;
	}
	Ref<EditorExportPlatform> platform = preset->get_platform();

	String option = args["option"];
	Variant value = args["value"];

	Dictionary result_data;
	result_data["preset"] = preset->get_name();
	result_data["option"] = option;
	bool filter_related = false;

	if (option == "name") {
		String new_name = String(value).strip_edges();
		if (new_name.is_empty()) {
			return ai_create_error_result(AIErrorCodes::INVALID_ARGS, "name must be a non-empty string");
		}
		EditorExport *ee = EditorExport::get_singleton();
		for (int i = 0; i < ee->get_export_preset_count(); i++) {
			Ref<EditorExportPreset> other = ee->get_export_preset(i);
			if (other.is_valid() && other != preset && other->get_name() == new_name) {
				return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
					vformat("An export preset named '%s' already exists.", new_name));
			}
		}
		result_data["old_value"] = preset->get_name();
		preset->set_name(new_name);
		result_data["new_value"] = new_name;
	} else if (option == "export_path") {
		String path = value;
		if (!path.is_empty() && platform.is_valid()) {
			String ext = path.get_extension().to_lower();
			List<String> valid_exts = platform->get_binary_extensions(preset);
			bool ext_ok = false;
			PackedStringArray ext_list;
			for (const String &e : valid_exts) {
				ext_list.push_back(e);
				if (ext == e.to_lower()) {
					ext_ok = true;
				}
			}
			if (!ext_ok) {
				Dictionary details;
				details["valid_extensions"] = ext_list;
				return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
					vformat("export_path extension '.%s' is not valid for platform '%s'.", ext, platform->get_name()),
					details);
			}
		}
		result_data["old_value"] = preset->get_export_path();
		preset->set_export_path(path);
		result_data["new_value"] = preset->get_export_path();
	} else if (option == "runnable") {
		result_data["old_value"] = preset->is_runnable();
		preset->set_runnable(value.booleanize());
		result_data["new_value"] = preset->is_runnable();
	} else if (option == "export_filter") {
		EditorExportPreset::ExportFilter filter;
		if (!_export_filter_from_string(value, filter)) {
			return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
				vformat("Invalid export_filter '%s'. Valid values: %s.", value, EXPORT_FILTER_VALUES));
		}
		result_data["old_value"] = _export_filter_to_string(preset->get_export_filter());
		preset->set_export_filter(filter);
		result_data["new_value"] = _export_filter_to_string(filter);
		filter_related = true;
	} else if (option == "include_filter" || option == "exclude_filter") {
		String filter_str = value;
		if (option == "include_filter") {
			result_data["old_value"] = preset->get_include_filter();
			preset->set_include_filter(filter_str);
		} else {
			result_data["old_value"] = preset->get_exclude_filter();
			preset->set_exclude_filter(filter_str);
		}
		result_data["new_value"] = filter_str;
		filter_related = true;
	} else if (option == "export_files") {
		if (value.get_type() != Variant::ARRAY) {
			return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
				"export_files must be an array of res:// paths");
		}
		Array files = value;
		Vector<String> new_files;
		for (int i = 0; i < files.size(); i++) {
			String path = files[i];
			if (!path.begins_with("res://")) {
				path = "res://" + path;
			}
			if (!FileAccess::exists(path)) {
				return ai_create_error_result(AIErrorCodes::FILE_NOT_FOUND,
					vformat("export_files entry '%s' does not exist.", path));
			}
			new_files.push_back(path);
		}
		Vector<String> old_files = preset->get_files_to_export();
		result_data["old_value"] = old_files.size();
		// Full replace.
		for (const String &old_file : old_files) {
			preset->remove_export_file(old_file);
		}
		for (const String &new_file : new_files) {
			preset->add_export_file(new_file);
		}
		result_data["new_value"] = new_files.size();
		EditorExportPreset::ExportFilter filter = preset->get_export_filter();
		if (filter != EditorExportPreset::EXPORT_SELECTED_SCENES &&
				filter != EditorExportPreset::EXPORT_SELECTED_RESOURCES &&
				filter != EditorExportPreset::EXCLUDE_SELECTED_RESOURCES) {
			Array warnings;
			warnings.push_back(vformat("export_files only applies when export_filter is 'scenes', 'resources', or 'exclude' — this preset's filter is '%s', so the list is currently ignored.", _export_filter_to_string(filter)));
			result_data["warnings"] = warnings;
		}
		filter_related = true;
	} else {
		// Platform option. The engine's _set() stores ANY key silently, so
		// validate against the preset's declared properties ourselves.
		StringName option_sn(option);
		const HashMap<StringName, PropertyInfo> &props = preset->get_properties();
		if (!props.has(option_sn)) {
			// Suggest near-matches across structural + platform option names.
			Vector<String> candidates;
			candidates.push_back("name");
			candidates.push_back("export_path");
			candidates.push_back("runnable");
			candidates.push_back("export_filter");
			candidates.push_back("include_filter");
			candidates.push_back("exclude_filter");
			candidates.push_back("export_files");
			for (const KeyValue<StringName, PropertyInfo> &E : props) {
				candidates.push_back(String(E.key));
			}
			PackedStringArray suggestions;
			for (const String &candidate : candidates) {
				if (candidate.findn(option) != -1 || option.findn(candidate) != -1 ||
						candidate.similarity(option) > 0.4f) {
					suggestions.push_back(candidate);
					if (suggestions.size() >= 5) {
						break;
					}
				}
			}
			Dictionary details;
			details["suggestions"] = suggestions;
			details["hint"] = "Option names are platform-specific. Structural keys: name, export_path, runnable, export_filter, include_filter, exclude_filter, export_files.";
			return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
				vformat("Unknown option '%s' for preset '%s' (platform '%s').", option, preset->get_name(),
					platform.is_valid() ? platform->get_name() : "unknown"),
				details);
		}
		const PropertyInfo &pinfo = props[option_sn];
		// Coerce common JSON-side types to the declared property type.
		Variant coerced = value;
		switch (pinfo.type) {
			case Variant::BOOL:
				coerced = value.booleanize();
				break;
			case Variant::INT:
				coerced = (int64_t)value;
				break;
			case Variant::FLOAT:
				coerced = (double)value;
				break;
			case Variant::STRING:
				coerced = String(value);
				break;
			default:
				break; // Pass through; _set stores the Variant as-is.
		}
		result_data["old_value"] = preset->get(option_sn);
		preset->set(option_sn, coerced);
		result_data["new_value"] = preset->get(option_sn);
	}

	if (filter_related) {
		Array warnings = result_data.has("warnings") ? (Array)result_data["warnings"] : Array();
		Array misconfig = _preset_misconfig_warnings(preset);
		for (int i = 0; i < misconfig.size(); i++) {
			warnings.push_back(misconfig[i]);
		}
		if (!warnings.is_empty()) {
			result_data["warnings"] = warnings;
		}
	}
	result_data["note"] = "res://export_presets.cfg rewritten (debounced ~0.8s). Not undoable, same as set_project_setting.";

	print_line(vformat("AI: Executed set_export_preset_option. Preset: %s, option: %s", preset->get_name(), option));
	return ai_create_success_result(result_data);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

// ==========================================================================
// Tier 2 — export and verify
// ==========================================================================

Dictionary exec_export_project(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	if (!args.has("preset") || args["preset"].get_type() != Variant::STRING) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"'preset' must be a string (the preset name)");
	}

	Dictionary error;
	Ref<EditorExportPreset> preset = _find_preset(args["preset"], error);
	if (preset.is_null()) {
		return error;
	}
	Ref<EditorExportPlatform> platform = preset->get_platform();
	if (platform.is_null()) {
		return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			vformat("Preset '%s' has no export platform.", preset->get_name()));
	}

	bool debug = args.get("debug", false);

	String output_path = args.get("output_path", String());
	if (output_path.is_empty()) {
		output_path = preset->get_export_path();
	}
	if (output_path.is_empty()) {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			"No output path: the preset has no export_path and no output_path was given. Pass output_path (e.g. 'builds/web/index.html') or set the preset's export_path.");
	}

	String ext = output_path.get_extension().to_lower();
	List<String> valid_exts = platform->get_binary_extensions(preset);
	bool ext_ok = false;
	PackedStringArray ext_list;
	for (const String &e : valid_exts) {
		ext_list.push_back(e);
		if (ext == e.to_lower()) {
			ext_ok = true;
		}
	}
	if (!ext_ok) {
		Dictionary details;
		details["valid_extensions"] = ext_list;
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			vformat("Output extension '.%s' is not valid for platform '%s'.", ext, platform->get_name()), details);
	}

	String abs_path = _resolve_output_path(output_path);

	// Pre-flight: platform validation.
	String export_error;
	bool missing_templates = false;
	if (!platform->can_export(preset, export_error, missing_templates, debug)) {
		Dictionary details;
		details["export_errors"] = export_error.strip_edges().split("\n");
		details["missing_templates"] = missing_templates;
		if (missing_templates) {
			details["hint"] = "Install export templates with install_export_templates (one-time per engine version).";
		}
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("Preset '%s' cannot export.", preset->get_name()), details);
	}

	// Pre-flight: content checks the engine skips. The empty-list case is a
	// hard block — an export provably containing no scenes is never intended.
	EditorExportPreset::ExportFilter filter = preset->get_export_filter();
	bool selective = filter == EditorExportPreset::EXPORT_SELECTED_SCENES ||
			filter == EditorExportPreset::EXPORT_SELECTED_RESOURCES;
	if (selective && preset->get_files_to_export().is_empty()) {
		Dictionary details;
		details["hint"] = vformat("Either set export_files (set_export_preset_option with option 'export_files') or set export_filter to 'all_resources'. Current filter: '%s'.", _export_filter_to_string(filter));
		return ai_create_error_result("empty_export_file_list",
			"export_filter is in selective mode but export_files is empty — this would silently export only autoloads (a near-empty pck). Refusing.",
			details);
	}
	Array warnings = _preset_misconfig_warnings(preset);

	// The export dialog's file picker normally guarantees the directory
	// exists; create it defensively for tool-driven exports.
	Error dir_err = DirAccess::make_dir_recursive_absolute(abs_path.get_base_dir());
	if (dir_err != OK && dir_err != ERR_ALREADY_EXISTS) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("Could not create output directory '%s': %s", abs_path.get_base_dir(), _error_to_string(dir_err)));
	}

	ai_log_verbose(vformat("Exporting preset '%s' to '%s' (debug=%s)", preset->get_name(), abs_path, debug ? "true" : "false"));

	platform->clear_messages();
	uint64_t start_ms = OS::get_singleton()->get_ticks_msec();
	Error err = platform->export_project(preset, debug, abs_path, 0);
	uint64_t elapsed_ms = OS::get_singleton()->get_ticks_msec() - start_ms;

	Array messages;
	for (int i = 0; i < platform->get_message_count(); i++) {
		EditorExportPlatform::ExportMessage msg = platform->get_message(i);
		Dictionary m;
		switch (msg.msg_type) {
			case EditorExportPlatform::EXPORT_MESSAGE_ERROR:
				m["type"] = "error";
				break;
			case EditorExportPlatform::EXPORT_MESSAGE_WARNING:
				m["type"] = "warning";
				break;
			default:
				m["type"] = "info";
				break;
		}
		m["category"] = msg.category;
		m["text"] = msg.text;
		messages.push_back(m);
	}

	if (err != OK) {
		Dictionary details;
		details["messages"] = messages;
		details["error"] = _error_to_string(err);
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("Export failed: %s", _error_to_string(err)), details);
	}

	// Post-export verification: artifact listing + pck tripwire.
	Dictionary result_data;
	result_data["output_path"] = abs_path;
	result_data["debug"] = debug;
	result_data["elapsed_ms"] = (int64_t)elapsed_ms;

	String base_dir = abs_path.get_base_dir();
	String basename = abs_path.get_file().get_basename();
	Array files;
	int64_t total_size = 0;
	Ref<DirAccess> da = DirAccess::open(base_dir);
	if (da.is_valid()) {
		da->list_dir_begin();
		String fname = da->get_next();
		while (!fname.is_empty()) {
			if (!da->current_is_dir() &&
					(fname == abs_path.get_file() || fname.begins_with(basename + "."))) {
				Dictionary fentry;
				fentry["name"] = fname;
				int64_t fsize = 0;
				{
					Ref<FileAccess> fa = FileAccess::open(base_dir.path_join(fname), FileAccess::READ);
					if (fa.is_valid()) {
						fsize = (int64_t)fa->get_length();
					}
				}
				fentry["size_bytes"] = fsize;
				total_size += fsize;
				files.push_back(fentry);
			}
			fname = da->get_next();
		}
		da->list_dir_end();
	}
	result_data["files"] = files;
	result_data["total_size_bytes"] = total_size;

	String pck_path = base_dir.path_join(basename + ".pck");
	if (FileAccess::exists(pck_path)) {
		int64_t pck_count = _read_pck_file_count(pck_path);
		result_data["pck_file_count"] = pck_count;
		if (pck_count >= 0 && pck_count < 10) {
			warnings.push_back(vformat(
				"The exported pck contains only %d embedded files — suspiciously few. This usually means only autoloads were exported (check export_filter / export_files).",
				(int)pck_count));
		}
	} else if (preset->has("binary_format/embed_pck") && (bool)preset->get("binary_format/embed_pck")) {
		result_data["pck_note"] = "pck is embedded in the binary — embedded file count unavailable.";
	}

	if (!warnings.is_empty()) {
		result_data["warnings"] = warnings;
	}
	result_data["messages"] = messages;

	print_line(vformat("AI: Executed export_project. Preset: %s -> %s (%d files)", preset->get_name(), abs_path, files.size()));
	return ai_create_success_result(result_data);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

Dictionary exec_serve_web_build(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	String action = args.get("action", "serve_and_open");
	if (action != "serve" && action != "serve_and_open" && action != "stop") {
		return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
			vformat("Invalid action '%s'. Valid: serve, serve_and_open, stop.", action));
	}

	Dictionary error;
	Ref<EditorExportPlatform> platform = _find_platform("Web", error);
	if (platform.is_null()) {
		return error;
	}

	EditorExport *ee = EditorExport::get_singleton();

	// Resolve the preset. poll_export() keys its state off the FIRST runnable
	// Web preset, so default to the same selection.
	Ref<EditorExportPreset> preset;
	String preset_name = args.get("preset", String());
	if (!preset_name.is_empty()) {
		preset = _find_preset(preset_name, error);
		if (preset.is_null()) {
			return error;
		}
		if (preset->get_platform() != platform) {
			return ai_create_error_result(AIErrorCodes::INVALID_ARGS,
				vformat("Preset '%s' is not a Web preset.", preset->get_name()));
		}
	} else {
		for (int i = 0; i < ee->get_export_preset_count(); i++) {
			Ref<EditorExportPreset> candidate = ee->get_export_preset(i);
			if (candidate.is_valid() && candidate->is_runnable() && candidate->get_platform() == platform) {
				preset = candidate;
				break;
			}
		}
	}

	if (action == "stop") {
		// poll_export() stops a listening server as a side effect whenever the
		// resulting state is not SERVING; with state SERVING, run() option 2
		// stops it explicitly. Either way no preset content is required.
		platform->poll_export();
		int options_count = platform->get_options_count();
		if (options_count >= 3 && preset.is_valid()) {
			platform->run(preset, 2, 0);
		}
		Dictionary result_data;
		result_data["action"] = "stop";
		result_data["note"] = "Local web server stopped (or was not running).";
		print_line("AI: Executed serve_web_build. Action: stop");
		return ai_create_success_result(result_data);
	}

	if (preset.is_null()) {
		Dictionary details;
		details["hint"] = "Create one with create_export_preset (platform 'Web', runnable true), or mark an existing Web preset runnable via set_export_preset_option.";
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			"No runnable Web export preset found.", details);
	}
	if (!preset->is_runnable()) {
		Dictionary details;
		details["hint"] = vformat("Set it runnable first: set_export_preset_option(preset: '%s', option: 'runnable', value: true). The serve flow requires a runnable preset.", preset->get_name());
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("Preset '%s' is not runnable.", preset->get_name()), details);
	}

	// The serve flow exports a DEBUG build — validate against debug templates
	// for a friendly error instead of run()'s opaque FAILED.
	String export_error;
	bool missing_templates = false;
	if (!platform->can_export(preset, export_error, missing_templates, /*p_debug=*/true)) {
		Dictionary details;
		details["export_errors"] = export_error.strip_edges().split("\n");
		details["missing_templates"] = missing_templates;
		if (missing_templates) {
			details["hint"] = "Serving uses a DEBUG web build — debug templates must be installed (install_export_templates installs both).";
		}
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("Preset '%s' cannot export a debug web build.", preset->get_name()), details);
	}

	// Same hard block as export_project: a served build with no scenes helps nobody.
	EditorExportPreset::ExportFilter filter = preset->get_export_filter();
	bool selective = filter == EditorExportPreset::EXPORT_SELECTED_SCENES ||
			filter == EditorExportPreset::EXPORT_SELECTED_RESOURCES;
	if (selective && preset->get_files_to_export().is_empty()) {
		Dictionary details;
		details["hint"] = "Set export_files or switch export_filter to 'all_resources' first.";
		return ai_create_error_result("empty_export_file_list",
			"export_filter is in selective mode but export_files is empty — the served build would contain only autoloads. Refusing.",
			details);
	}
	Array warnings = _preset_misconfig_warnings(preset);

	// run() hard-fails unless poll_export() has raised the platform's remote
	// debug state. The state itself is private but readable through the
	// options count: 0 = unavailable, 2 = available, 3 = serving.
	platform->poll_export();
	int options_count = platform->get_options_count();
	if (options_count == 0) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			"Web remote-deploy state is unavailable despite passing pre-checks — the first runnable Web preset may differ from the requested one and fail can_export. Check get_export_status.");
	}

	int option = (action == "serve_and_open") ? 0 : 1;
	ai_log_verbose(vformat("serve_web_build: running preset '%s' option %d (options_count=%d)", preset->get_name(), option, options_count));
	Error err = platform->run(preset, option, 0);
	if (err != OK) {
		return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
			vformat("Web serve failed: %s", _error_to_string(err)));
	}

	bool use_tls = EDITOR_GET("export/web/use_tls");
	String host = EDITOR_GET("export/web/http_host");
	int port = EDITOR_GET("export/web/http_port");
	String url = vformat("%s://%s:%d/tmp_js_export.html", use_tls ? "https" : "http", host, port);

	Dictionary result_data;
	result_data["action"] = action;
	result_data["url"] = url;
	result_data["preset"] = preset->get_name();
	Array notes;
	notes.push_back("This is a DEBUG build exported to the editor's temp dir — separate from export_project output.");
	notes.push_back("The server sends the COOP/COEP headers web builds require, and keeps running until stopped (action 'stop') or the editor closes.");
	if (action == "serve") {
		notes.push_back("No browser was opened — give the user the URL.");
	}
	result_data["notes"] = notes;
	if (!warnings.is_empty()) {
		result_data["warnings"] = warnings;
	}

	print_line(vformat("AI: Executed serve_web_build. Action: %s, URL: %s", action, url));
	return ai_create_success_result(result_data);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

// ==========================================================================
// Tier 3 — templates
// ==========================================================================

Dictionary exec_open_template_manager(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	EditorNode *en = EditorNode::get_singleton();
	if (!en) {
		return ai_create_error_result(AIErrorCodes::INTERNAL_ERROR,
			"EditorNode singleton not available");
	}
	en->open_export_template_manager();
	Dictionary result_data;
	result_data["note"] = "Manage Export Templates dialog opened. For installing templates on this fork prefer install_export_templates — the dialog's online download will not find an era-matched package for this engine version.";
	print_line("AI: Executed open_template_manager.");
	return ai_create_success_result(result_data);
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

Dictionary exec_install_export_templates(const Dictionary &args) {
#ifdef TOOLS_ENABLED
	bool force = args.get("force", false);
	if (templates_installed() && !force) {
		Dictionary result_data;
		result_data["already_installed"] = true;
		result_data["path"] = get_templates_dir();
		result_data["file_count"] = count_template_files();
		result_data["note"] = "Export templates for this engine version are already installed. Pass force=true to reinstall.";
		return ai_create_success_result(result_data);
	}
	// The download+extract flow is asynchronous and driven by the agentic
	// orchestrator (it intercepts this tool by name before dispatch). Reaching
	// this branch means the tool was invoked outside the agent loop.
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Template installation requires the agent loop (asynchronous download). This code path should not normally be reached.");
#else
	return ai_create_error_result(AIErrorCodes::OPERATION_FAILED,
		"Editor API not available in non-editor builds");
#endif
}

} // namespace AIExportActions
