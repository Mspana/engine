// modules/ai/ai_journal_writer.cpp

#include "ai_journal_writer.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"

AIJournalWriter::AIJournalWriter() {
	_thread.start(_thread_function, this);
}

AIJournalWriter::~AIJournalWriter() {
	{
		MutexLock lock(_mutex);
		_stop = true;
	}
	_semaphore.post(); // Wake thread so it can observe _stop and exit
	_thread.wait_to_finish();
}

void AIJournalWriter::enqueue(const String &p_file_path, const Dictionary &p_entry) {
	Entry e;
	e.file_path = p_file_path;
	e.line = JSON::stringify(p_entry); // Serialize on caller thread (cheap, no I/O)
	MutexLock lock(_mutex);
	_queue.push_back(e);
	_semaphore.post();
}

void AIJournalWriter::_thread_function(void *p_self) {
	AIJournalWriter *self = static_cast<AIJournalWriter *>(p_self);

	while (true) {
		self->_semaphore.wait();

		List<Entry> to_write;
		{
			MutexLock lock(self->_mutex);
			if (self->_stop && self->_queue.is_empty()) {
				break;
			}
			to_write = self->_queue; // Drain under lock
			self->_queue.clear();
		}

		for (const Entry &e : to_write) {
			// Ensure the parent directory exists before writing
			DirAccess::make_dir_recursive_absolute(e.file_path.get_base_dir());

			Error err;
			// READ_WRITE ("rb+") opens without truncation — required for append.
			// It fails if the file doesn't exist yet, so create it first in that case.
			Ref<FileAccess> f = FileAccess::open(e.file_path, FileAccess::READ_WRITE, &err);
			if (!f.is_valid()) {
				// File doesn't exist — create it, then reopen for read-write
				Ref<FileAccess> create_f = FileAccess::open(e.file_path, FileAccess::WRITE, &err);
				if (create_f.is_valid()) {
					create_f.unref();
					f = FileAccess::open(e.file_path, FileAccess::READ_WRITE, &err);
				}
			}
			if (f.is_valid()) {
				f->seek_end();
				f->store_line(e.line);
				f->flush();
			}
		}
	}
}
