// modules/ai/template_installer.h
// Shared async export-template installer (download + threaded extraction),
// used by the codex harness driver. The legacy orchestrator carries its own
// copy of this machinery, interwoven with its run-loop state; that copy is
// retired together with the orchestrator rather than refactored onto this
// class (churn on code scheduled for deletion).

#ifndef AI_TEMPLATE_INSTALLER_H
#define AI_TEMPLATE_INSTALLER_H

#include "core/object/ref_counted.h"

struct AITplExtractContext;

class AITemplateInstaller : public RefCounted {
	GDCLASS(AITemplateInstaller, RefCounted);

public:
	// Begins the download+extract flow. p_progress is called with a status
	// string as work advances; p_done exactly once with the exec-result
	// Dictionary ({status, result|error}) unless cancel() ran first.
	// Returns false (and calls nothing) if a flow is already active.
	bool start(const Callable &p_progress, const Callable &p_done);
	// Silent teardown: aborts the download / signals the extraction worker to
	// stop. p_done is NOT called.
	void cancel();
	bool is_active() const { return phase != PHASE_INACTIVE; }

	~AITemplateInstaller();

private:
	enum Phase {
		PHASE_INACTIVE,
		PHASE_DOWNLOADING,
		PHASE_EXTRACTING,
	};

	Phase phase = PHASE_INACTIVE;
	Callable progress_cb;
	Callable done_cb;
	ObjectID http_id;
	String tmp_path;
	uint64_t start_ms = 0;
	uint64_t last_progress_ms = 0;
	int64_t last_bytes = 0;
	uint32_t tick_gen = 0;
	AITplExtractContext *extract_ctx = nullptr;

	void _schedule_tick(float p_delay);
	void _tick_gen_cb(uint32_t p_gen);
	void _tick();
	void _on_download_completed(int p_result, int p_response_code, const PackedStringArray &p_headers, const PackedByteArray &p_body);
	void _finish(const Dictionary &p_exec_result);
	void _release_extract_ctx(bool p_cancel);
	void _cleanup_tmp();
};

#endif // AI_TEMPLATE_INSTALLER_H
