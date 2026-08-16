#pragma once

#include "communication.pb.h"

#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <libpq-fe.h>
#endif

namespace scheduler {

// Framework-level persistence (PostgreSQL via libpq): runs and tasks tables.
// Writes are best-effort; failures are logged, never fatal. Thread-safe.
class Storage {
public:
    Storage() = default;
    ~Storage();

    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    // Connects and creates the framework tables. Returns false on failure.
    bool init(const std::string& conninfo);

    bool enabled() const {
        return conn_ != nullptr;
    }

    void SaveRun(const std::string& run_id, const std::string& plugin_type,
                 const std::string& dataset_version, const std::string& params);
    void FinishRun(const std::string& run_id, const std::string& status);
    std::vector<std::string> LoadUnfinishedRunIds();
    void SaveTask(const std::string& task_id, const std::string& run_id,
                  const std::string& client_identity, const std::string& payload);
    void AckTask(const std::string& task_id);
    void ReportTask(const std::string& task_id, const common::TaskResult& result);

private:
    struct PgParam {
        std::string value;
        bool binary = false;
    };
    bool Exec(const char* sql, const std::vector<PgParam>& params);

#ifdef _WIN32
    PGconn* conn_ = nullptr;
#else
    void* conn_ = nullptr;
#endif
    mutable std::mutex mutex_;
};

}  // namespace scheduler
