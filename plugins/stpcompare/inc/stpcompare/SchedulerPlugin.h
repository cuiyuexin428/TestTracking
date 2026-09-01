#pragma once

#include "common/TaskPlugin.h"
#include "stpcompare/StpStore.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace stpcompare {

// Scheduler-side stpcompare plugin: distributes stp files from a manifest to
// clients as a flat pool of tasks (no waves / settlement). Each task holds a
// batch of files; the client runs the configured converter per file and
// writes the two txt outputs straight into a configurable table. The server
// only tracks completion and persists a resume checkpoint.
class SchedulerPlugin : public common::TaskPlugin {
public:
    std::string pluginType() const override;
    bool init(const std::string& config) override;
    bool validateClient(const common::ClientInfo& client, const std::string& client_message,
                        common::ValidationResult& result) override;
    bool plan(const common::RunRequest& request,
              const std::vector<common::ClientInfo>& clients) override;
    bool requestTask(const common::ClientInfo& client, const std::string& request_payload,
                     common::Task& task) override;
    void processResult(const common::TaskResult& result) override;
    bool runFinished() override;
    void cancel() override;
    void requeue(const std::vector<std::string>& task_ids) override;
    bool saveCheckpoint(const std::string& run_id) override;
    bool restore(const std::string& run_id) override;

private:
    bool BuildTasks(const std::string& run_id);
    std::string ManifestFingerprint() const;
    void SaveCheckpointLocked();

    std::mutex mutex_;
    std::string run_id_;
    std::string manifest_path_;
    std::string manifest_fingerprint_;
    std::string expected_program_version_;
    std::string expected_dataset_version_;
    std::unique_ptr<StpCheckpointStore> store_;
    int task_size_ = 25;

    std::vector<common::Task> pending_;
    std::unordered_set<std::string> pending_ids_;
    std::unordered_map<std::string, common::Task> tasks_by_id_;
    std::unordered_map<std::string, std::string> task_client_;
    std::unordered_set<std::string> done_;
    std::vector<std::string> all_files_;
    int total_files_ = 0;
    int total_tasks_ = 0;
    bool last_checkpoint_save_ok_ = false;
};

}  // namespace stpcompare
