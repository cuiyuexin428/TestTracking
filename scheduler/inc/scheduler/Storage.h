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
    };

    Storage() = default;
    ~Storage() = default;

    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    // Connects and creates the framework tables. Returns false on failure.
    bool init(const std::string& conninfo);

    bool enabled() const { return enabled_; }

    bool SaveRun(const std::string& run_id, const std::string& plugin_type,
                 const std::string& dataset_version, const std::string& params);
    bool ActivateRun(const std::string& run_id, const std::string& plugin_type);
    bool FinishRun(const std::string& run_id, const std::string& status);
    bool LoadUnfinishedRuns(std::vector<RunRecord>& runs);
    bool ResumeRun(const std::string& run_id);
    bool SaveTask(const std::string& task_id, const std::string& run_id,
                  const std::string& client_identity, const std::string& payload,
                  const std::string& attempt_id);
    bool AckTask(const std::string& task_id);
    bool ReportTask(const std::string& task_id, const common::TaskResult& result);
    bool ReconcileCompletedTasks(const std::string& run_id, const std::string& plugin_type);
    bool ReclaimTasks(const std::vector<std::string>& task_ids);

   private:
    bool Exec(const char* sql, const std::vector<common::PgParam>& params);
    common::PgConnection connection_;
    bool enabled_ = false;
    mutable std::mutex mutex_;
};

}  // namespace scheduler
