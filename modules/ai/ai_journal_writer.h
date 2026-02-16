// modules/ai/ai_journal_writer.h
// Thread-safe, append-only JSONL writer for the AI journal system.

#ifndef AI_JOURNAL_WRITER_H
#define AI_JOURNAL_WRITER_H

#include "core/os/mutex.h"
#include "core/os/semaphore.h"
#include "core/os/thread.h"
#include "core/templates/list.h"
#include "core/variant/dictionary.h"

// Schema-agnostic JSONL appender. Serializes each Dictionary entry as a
// compact JSON line and writes it on a dedicated background thread.
// enqueue() is non-blocking and safe to call from any thread.
class AIJournalWriter {
	struct Entry {
		String file_path; // Absolute filesystem path
		String line;      // Pre-serialized JSON (no trailing newline)
	};

	Thread _thread;
	Mutex _mutex;
	Semaphore _semaphore;
	List<Entry> _queue;
	bool _stop = false;

	static void _thread_function(void *p_self);

public:
	AIJournalWriter();
	~AIJournalWriter();

	// Serialize p_entry to JSON and queue it for appending to p_file_path.
	// p_file_path must be an absolute path — resolve user:// before calling.
	void enqueue(const String &p_file_path, const Dictionary &p_entry);
};

#endif // AI_JOURNAL_WRITER_H
