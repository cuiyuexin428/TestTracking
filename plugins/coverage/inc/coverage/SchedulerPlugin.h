#pragma once

#include "common/TaskPlugin.h"
#include "coverage/CoverageStore.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coverage_plugin {

// Scheduler-side coverage plugin: owns the task pool (planned as waves),
// validates clients during registration, answers task requests with one task,
// and settles each wave with a line-level greedy selection over the collected
// coverage point sets. Business payloads are opaque to the framework.
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
    bool BuildWaves(const std::string& run_id);
    void SaveCheckpointLocked();
    // Settles the current wave and advances to the next one. Returns the
    // settled wave index, or -1 when settlement/merge failed (the run is
    // suspended with staging intact so it can be resumed).
    int SettleWave();
    bool MergeWaveCoverage(int wave);
    std::string ManifestFingerprint() const;

    std::mutex mutex_;
    std::string run_id_;
    std::string manifest_path_;
    std::string manifest_fingerprint_;
    std::string expected_program_version_;
    std::string expected_dataset_version_;
    std::string opencppcoverage_;
    std::unique_ptr<CoverageStore> store_;
    int batch_size_ = 1000;
    int task_size_ = 25;
    int failed_file_retries_ = 0;
    bool keep_staging_ = false;  // validation only: keep wave point files
    bool store_cov_ = false;     // wave settlement computes cov-based totals

    std::vector<std::vector<common::Task>> waves_;
    size_t current_wave_ = 0;
    size_t task_seq_ = 0;
    std::vector<common::Task> pending_;
    std::unordered_set<std::string> pending_ids_;
    // Reclaimed tasks (offline/lease) are re-sent before new tasks so the
    // current wave can settle sooner.
    std::vector<common::Task> priority_pending_;
    std::unordered_set<std::string> priority_ids_;
    std::unordered_set<std::string> wave_task_ids_;
    std::unordered_map<std::string, std::string> task_client_;
    std::unordered_map<std::string, common::Task> tasks_by_id_;
    std::set<std::uint64_t> cumulative_;
    size_t total_files_ = 0;
    size_t selected_count_ = 0;
};

}  // namespace coverage_plugin
