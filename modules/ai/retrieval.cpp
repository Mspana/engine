// modules/ai/retrieval.cpp
#include "retrieval.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/error/error_macros.h"
#include "core/object/class_db.h"

RetrievalIndex::RetrievalIndex() {
	index_built = false;
}

RetrievalIndex::~RetrievalIndex() {
}

void RetrievalIndex::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_project_root", "root"), &RetrievalIndex::set_project_root);
	ClassDB::bind_method(D_METHOD("get_project_root"), &RetrievalIndex::get_project_root);
	ClassDB::bind_method(D_METHOD("build_index"), &RetrievalIndex::build_index);
	ClassDB::bind_method(D_METHOD("retrieve_context", "query", "active_scene_path", "top_k"), &RetrievalIndex::retrieve_context, DEFVAL(8));

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "project_root"), "set_project_root", "get_project_root");
}

void RetrievalIndex::set_project_root(const String &p_root) {
	project_root = p_root;
}

String RetrievalIndex::get_project_root() const {
	return project_root;
}

bool RetrievalIndex::_should_index_file(const String &file_path) const {
	// Check if file has one of the extensions we want to index
	return file_path.ends_with(".gd") || 
	       file_path.ends_with(".cs") || 
	       file_path.ends_with(".tscn") || 
	       file_path.ends_with(".tres");
}

void RetrievalIndex::_scan_directory(const String &dir_path) {
	Ref<DirAccess> dir = DirAccess::open(dir_path);
	if (dir.is_null()) {
		WARN_PRINT(vformat("RetrievalIndex: Cannot open directory: %s", dir_path));
		return;
	}

	dir->list_dir_begin();
	String file_name = dir->get_next();

	while (!file_name.is_empty()) {
		// Skip hidden files and special directories
		if (file_name.begins_with(".")) {
			file_name = dir->get_next();
			continue;
		}

		String full_path = dir_path.path_join(file_name);

		if (dir->current_is_dir()) {
			// Skip common directories that shouldn't be indexed
			if (file_name != "addons" && file_name != ".godot" && file_name != ".git") {
				_scan_directory(full_path);
			}
		} else if (_should_index_file(file_name)) {
			// Read and index this file
			Ref<FileAccess> file = FileAccess::open(full_path, FileAccess::READ);
			if (file.is_null()) {
				WARN_PRINT(vformat("RetrievalIndex: Cannot read file: %s", full_path));
			} else {
				// Limit file size to ~200KB
				const int64_t MAX_FILE_SIZE = 200 * 1024;
				int64_t file_length = file->get_length();
				int64_t bytes_to_read = MIN(file_length, MAX_FILE_SIZE);

				PackedByteArray buffer = file->get_buffer(bytes_to_read);
				String content = String::utf8((const char *)buffer.ptr(), buffer.size());

				// Create and store snippet
				Snippet snippet;
				snippet.file_path = full_path;
				snippet.text = content;
				snippet.score = 0.0;
				snippet_index.push_back(snippet);
			}
		}

		file_name = dir->get_next();
	}

	dir->list_dir_end();
}

void RetrievalIndex::build_index() {
	if (index_built) {
		return; // Already built
	}

	if (project_root.is_empty()) {
		WARN_PRINT("RetrievalIndex: Cannot build index - project_root is not set");
		return;
	}

	print_line(vformat("RetrievalIndex: Building index for project: %s", project_root));

	snippet_index.clear();
	_scan_directory(project_root);

	index_built = true;
	print_line(vformat("RetrievalIndex: Index built with %d files", snippet_index.size()));
}

Array RetrievalIndex::retrieve_context(const String &query, const String &active_scene_path, int32_t top_k) {
	// Ensure index is built
	if (!index_built) {
		build_index();
	}

	Array result;

	if (snippet_index.is_empty()) {
		return result; // No snippets to search
	}

	// Tokenize query into keywords (lowercase, split on whitespace, filter short tokens)
	String query_lower = query.to_lower();
	Vector<String> keywords;
	PackedStringArray query_parts = query_lower.split(" ", false);
	
	for (int i = 0; i < query_parts.size(); i++) {
		String token = query_parts[i].strip_edges();
		if (token.length() >= 3) {
			keywords.push_back(token);
		}
	}

	if (keywords.is_empty()) {
		return result; // No valid keywords to search
	}

	// Score each snippet
	Vector<Snippet> scored_snippets = snippet_index; // Make a copy for scoring
	
	for (int i = 0; i < scored_snippets.size(); i++) {
		real_t score = 0.0;
		String snippet_text_lower = scored_snippets[i].text.to_lower();
		String snippet_path_lower = scored_snippets[i].file_path.to_lower();

		// Count keyword occurrences
		for (int k = 0; k < keywords.size(); k++) {
			const String &keyword = keywords[k];
			
			// Count occurrences in text
			int pos = 0;
			while ((pos = snippet_text_lower.find(keyword, pos)) != -1) {
				score += 1.0;
				pos += keyword.length();
			}

			// Bonus for keyword in file path
			if (snippet_path_lower.contains(keyword)) {
				score += 2.0;
			}
		}

		// Bonus if snippet is related to active scene
		if (!active_scene_path.is_empty()) {
			String active_scene_lower = active_scene_path.to_lower();
			String scene_base_name = active_scene_path.get_file().get_basename().to_lower();
			String snippet_base_name = scored_snippets[i].file_path.get_file().get_basename().to_lower();

			// If same base name or same directory, add bonus
			if (snippet_base_name == scene_base_name) {
				score += 5.0;
			} else if (scored_snippets[i].file_path.get_base_dir() == active_scene_path.get_base_dir()) {
				score += 2.0;
			}
		}

		scored_snippets.write[i].score = score;
	}

	// Sort snippets by score (descending)
	// Simple bubble sort for small arrays
	for (int i = 0; i < scored_snippets.size() - 1; i++) {
		for (int j = 0; j < scored_snippets.size() - i - 1; j++) {
			if (scored_snippets[j].score < scored_snippets[j + 1].score) {
				// Swap
				Snippet temp = scored_snippets[j];
				scored_snippets.write[j] = scored_snippets[j + 1];
				scored_snippets.write[j + 1] = temp;
			}
		}
	}

	// Take top K snippets with score > 0
	int count = 0;
	for (int i = 0; i < scored_snippets.size() && count < top_k; i++) {
		if (scored_snippets[i].score > 0.0) {
			Dictionary snippet_dict;
			snippet_dict["file_path"] = scored_snippets[i].file_path;
			
			// Truncate text to ~3000 characters to avoid huge prompts
			String snippet_text = scored_snippets[i].text;
			const int MAX_SNIPPET_LENGTH = 3000;
			if (snippet_text.length() > MAX_SNIPPET_LENGTH) {
				snippet_text = snippet_text.substr(0, MAX_SNIPPET_LENGTH) + "\n... (truncated)";
			}
			
			snippet_dict["text"] = snippet_text;
			snippet_dict["score"] = scored_snippets[i].score;
			
			result.push_back(snippet_dict);
			count++;
		}
	}

	return result;
}

