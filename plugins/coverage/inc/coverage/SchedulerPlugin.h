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

// Scheduler-side coverage plugin: batches the stp file set into tasks,
// gates dispatch to the current wave, and settles each wave with a line-level
// greedy selection over the collected coverage point sets.
class SchedulerPlugin : public common::TaskPlugin {
public:
    std::string taskType() const override;
    bool init(const std::string& config) override;
    bool plan(const common::RunRequest& request,
              const std::vector<common::ClientInfo>& clients) override;
    bool dispatch(const std::vector<common::ClientInfo>& clients,
                  std::vector<common::TaskAssignment>& out) override;
    void requeue(const std::vector<std::string>& task_ids) override;
    void processResult(const common::TaskResult& result) override;

private:
    void SettleWave();

    std::mutex mutex_;
    std::string run_id_;
    std::string manifest_path_;
    std::unique_ptr<CoverageStore> store_;
    int batch_size_ = 1000;
    int task_size_ = 25;
    bool keep_staging_ = false;  // validation only: keep wave point files

    std::vector<std::vector<common::TaskSpec>> waves_;
    size_t current_wave_ = 0;
    std::vector<common::TaskSpec> pending_;
    std::unordered_set<std::string> wave_task_ids_;
    std::unordered_map<std::string, std::string> task_client_;
    std::unordered_map<std::string, int> client_tasks_;
    std::unordered_map<std::string, common::TaskSpec> tasks_by_id_;
    std::set<std::uint64_t> cumulative_;
    size_t total_files_ = 0;
    size_t selected_count_ = 0;
};

}  // namespace coverage_plugin
