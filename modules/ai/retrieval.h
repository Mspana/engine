#ifndef AI_RETRIEVAL_H
#define AI_RETRIEVAL_H

#include "core/object/ref_counted.h"
#include "core/string/ustring.h"
#include "core/variant/array.h"
#include "core/templates/vector.h"

class RetrievalIndex : public RefCounted {
	GDCLASS(RetrievalIndex, RefCounted);

public:
	struct Snippet {
		String file_path;
		String text;
		real_t score;

		Snippet() : score(0.0) {}
	};

private:
	Vector<Snippet> snippet_index;
	String project_root;
	bool index_built;

	// Helper to recursively scan directories
	void _scan_directory(const String &dir_path);

	// Helper to check if file extension should be indexed
	bool _should_index_file(const String &file_path) const;

protected:
	static void _bind_methods();

public:
	void set_project_root(const String &p_root);
	String get_project_root() const;

	void build_index(); // no-op if already built
	Array retrieve_context(const String &query, const String &active_scene_path, int32_t top_k = 8);

	RetrievalIndex();
	~RetrievalIndex();
};

#endif // AI_RETRIEVAL_H

