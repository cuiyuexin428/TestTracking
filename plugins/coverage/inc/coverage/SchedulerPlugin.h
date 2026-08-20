#pragma once

#include "common/TaskPlugin.h"
#include "coverage/CoverageStore.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
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
//
// Result handling and wave settlement are decoupled: result bookkeeping only
// touches a short critical section (mutex_), while settlement runs on the
// triggering result worker through a single settlement loop (settle_queue_ +
// settle_active_) that serializes waves in order. The heavy merge does not
// hold the plugin mutex, so dispatch, heartbeats and result processing stay
// responsive while a wave is being settled.
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
    // Handles one task result under mutex_. Returns true when the wave's task
    // pool became empty and the caller must run SettleWaveLoop().
    bool HandleResultLocked(const common::TaskResult& result);
    // Drains settle_queue_ in wave order. At most one loop is active at a time
    // (settle_active_); on failure the chain is paused at settle_failed_wave_.
    void SettleWaveLoop();
    // Settles one wave: greedy selection + cumulative update + cov merge.
    // Runs on the settlement loop thread without holding mutex_.
    bool SettleWave(int wave);
    bool MergeWaveCoverage(int wave);
    std::string ManifestFingerprint() const;

    // Dispatch/bookkeeping state; short critical sections only.
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
    bool keep_staging_ = false;  // validation only: keep wave point files
    bool store_cov_ = false;     // wave settlement computes cov-based totals
    bool last_checkpoint_save_ok_ = false;

    std::vector<std::vector<common::Task>> waves_;
    // Outstanding (not yet reported) task ids per wave. Every wave starts with
    // all of its tasks here; results erase ids, and an empty set means the
    // wave is ready to settle. Multiple waves can be in flight at once.
    std::unordered_map<int, std::unordered_set<std::string>> wave_outstanding_;
    // Wave index of every known task id (used by requeue/result routing).
    std::unordered_map<std::string, int> task_wave_;
    // Waves whose settlement completed successfully (always a prefix unless
    // the settlement chain paused at settle_failed_wave_).
    std::unordered_set<int> settled_waves_;
    std::vector<common::Task> pending_;
    std::unordered_set<std::string> pending_ids_;
    // Reclaimed tasks (offline/lease) are re-sent before new tasks so the
    // current wave can settle sooner.
    std::vector<common::Task> priority_pending_;
    std::unordered_set<std::string> priority_ids_;
    std::unordered_map<std::string, std::string> task_client_;
    std::unordered_map<std::string, common::Task> tasks_by_id_;
    std::set<std::uint64_t> cumulative_;
    size_t total_files_ = 0;

    // Settlement state. settle_queue_ is protected by settle_mutex_; the
    // active/failed flags are atomic so result bookkeeping never blocks on a
    // running merge. Lock order: mutex_ -> settle_mutex_ (never the reverse).
    std::mutex settle_mutex_;
    std::condition_variable settle_cv_;
    std::deque<int> settle_queue_;
    std::atomic<bool> settle_active_{false};
    std::atomic<int> settle_failed_wave_{-1};
    std::atomic<size_t> selected_count_{0};
};

}  // namespace coverage_plugin
