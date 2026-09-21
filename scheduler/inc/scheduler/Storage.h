#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "common/PgUtil.h"
#include "communication.pb.h"

#ifdef _WIN32
#include <libpq-fe.h>
#endif

namespace scheduler {

// Framework-level persistence (PostgreSQL via libpq): runs and tasks tables.
// Required writes return explicit commit status. Calls are serialized by the
// scheduler worker; the mutex also permits isolated storage tests.
class Storage {
   public:
    struct RunRecord {
        std::string run_id;
        std::string plugin_type;
        std::string dataset_version;
        std::string params;
        std::string work_fingerprint;
        bool force_rerun = false;
        std::string force_rerun_reason;
        std::string rerun_of;
        std::string status;
        std::vector<std::string> worker_ids;
        std::string execution_fingerprint;
    };

    struct EnqueueResult {
        enum class Outcome { Created, Duplicate, ForceTargetMissing, Error } outcome = Outcome::Error;
        std::string run_id;
        std::string status;
        std::string rerun_of;
    };

    Storage() = default;
    ~Storage() = default;

    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    // Connects and creates the framework tables. Returns false on failure.
    bool init(const std::string& conninfo);

    bool enabled() const { return enabled_; }

    EnqueueResult EnqueueRun(const RunRecord& run);
    bool BeginPlanning(const std::string& run_id, const std::string& plugin_type);
    bool ActivateRun(const std::string& run_id, const std::string& plugin_type);
    bool FinishRun(const std::string& run_id, const std::string& status);
    bool CancelQueuedRun(const std::string& run_id, const std::string& plugin_type);
    bool LoadUnfinishedRuns(std::vector<RunRecord>& active, std::vector<RunRecord>& queued);
    bool RestoreRun(const std::string& run_id, const std::string& plugin_type);
    bool SaveTask(const std::string& task_id, const std::string& run_id,
                  const std::string& client_identity, const std::string& payload,
                  const std::string& attempt_id);
    bool AckTask(const std::string& task_id);
    bool ReportTask(const std::string& task_id, const common::TaskResult& result);
    bool ReclaimTasks(const std::vector<std::string>& task_ids);

   private:
    bool Exec(const char* sql, const std::vector<common::PgParam>& params);
    common::PgConnection connection_;
    bool enabled_ = false;
    mutable std::mutex mutex_;
};

}  // namespace scheduler
