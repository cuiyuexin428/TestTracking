#include "coverage/SchedulerPlugin.h"

#include "common/Logger.h"

#include "coverage.pb.h"
#include "coverage/CoverageUtil.h"

#include <json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace coverage_plugin {

std::string SchedulerPlugin::pluginType() const {
    return "coverage";
}

bool SchedulerPlugin::init(const std::string& config) {
    const nlohmann::json cfg = nlohmann::json::parse(config);
    manifest_path_ = cfg.value("stp_manifest", std::string());
    expected_program_version_ = cfg.value("expected_program_version", std::string());
    expected_dataset_version_ = cfg.value("expected_dataset_version", std::string());
    batch_size_ = cfg.value("batch_size", 1000);
    task_size_ = cfg.value("task_size", 25);
    store_cov_ = cfg.value("store_cov", false);
    opencppcoverage_ = cfg.value("opencppcoverage", std::string("OpenCppCoverage.exe"));
    keep_staging_ = cfg.value("keep_staging", false);
    if (manifest_path_.empty()) {
        LOG_ERROR("coverage plugin: stp_manifest is required");
        return false;
    }
    if (batch_size_ <= 0 || task_size_ <= 0) {
        LOG_ERROR("coverage plugin: batch_size and task_size must be > 0");
        return false;
    }
    if (store_cov_) {
#ifdef _WIN32
        const bool is_bare_name =
            opencppcoverage_.find('\\') == std::string::npos &&
            opencppcoverage_.find('/') == std::string::npos &&
            opencppcoverage_.find(':') == std::string::npos;
        if (!is_bare_name) {
            std::error_code ec;
            if (!std::filesystem::exists(opencppcoverage_, ec)) {
                LOG_ERROR("coverage plugin: opencppcoverage not found: "
                          << opencppcoverage_);
                return false;
            }
        }
#else
        LOG_ERROR("coverage plugin: cov merge requires OpenCppCoverage (Windows only)");
        return false;
#endif
    }
    store_ = CreateCoverageStore(config);
    if (!store_->init(config)) {
        LOG_ERROR("coverage plugin: coverage store init failed");
        return false;
    }
    if (store_cov_) {
        LOG_INFO("coverage plugin: cov merge enabled, opencppcoverage=" << opencppcoverage_);
    }
    return true;
}

bool SchedulerPlugin::validateClient(const common::ClientInfo& client,
                                     const std::string& client_message,
                                     common::ValidationResult& result) {
    (void)client;

    coverage::CoverageClientInfo info;
    if (!info.ParseFromString(client_message)) {
        result.accepted = false;
        result.reason = "unparseable coverage client info";
        return true;
    }

    const bool version_ok =
        (expected_program_version_.empty() ||
         info.program_version() == expected_program_version_) &&
        (expected_dataset_version_.empty() ||
         info.dataset_version() == expected_dataset_version_);
    if (version_ok) {
        result.accepted = true;
        result.reason = "ok";
        return true;
    }

    result.accepted = false;
    result.reason = "version mismatch (program expected=" + expected_program_version_ +
                    " got=" + info.program_version() + ", dataset expected=" +
                    expected_dataset_version_ + " got=" + info.dataset_version() + ")";
    return true;
}

bool SchedulerPlugin::BuildWaves(const std::string& run_id) {
    std::vector<std::pair<std::string, std::uint64_t>> files;
    std::ifstream in(manifest_path_);
    if (!in.is_open()) {
        LOG_ERROR("coverage plugin: cannot open manifest " << manifest_path_);
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        // Strip a UTF-8 BOM; editors and PowerShell scripts commonly add one
        // to text manifests.
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);
        }
        if (line.empty()) {
            continue;
        }
        const size_t comma = line.find(',');
        if (comma == std::string::npos) {
            files.emplace_back(line, 0);
        } else {
            files.emplace_back(line.substr(0, comma),
                               std::strtoull(line.substr(comma + 1).c_str(), nullptr, 10));
        }
    }
    if (files.empty()) {
        LOG_ERROR("coverage plugin: manifest is empty");
        return false;
    }
    manifest_fingerprint_ = ManifestFingerprint();
    if (manifest_fingerprint_.empty()) {
        LOG_ERROR("coverage plugin: cannot fingerprint manifest " << manifest_path_);
        return false;
    }
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });

    waves_.clear();
    for (size_t start = 0; start < files.size(); start += static_cast<size_t>(batch_size_)) {
        const size_t end = std::min(start + static_cast<size_t>(batch_size_), files.size());
        std::vector<common::Task> wave_tasks;
        const size_t wave_index = waves_.size();
        for (size_t t = start; t < end; t += static_cast<size_t>(task_size_)) {
            const size_t tend = std::min(t + static_cast<size_t>(task_size_), end);
            coverage::CoverageTask task;
            task.set_run_id(run_id);
            task.set_wave_id(static_cast<std::int32_t>(wave_index));
            for (size_t i = t; i < tend; ++i) {
                task.add_files(files[i].first);
            }

            common::Task spec;
            spec.set_task_id(run_id + "-w" + std::to_string(wave_index) + "-t" +
                             std::to_string(wave_tasks.size()));
            task.SerializeToString(spec.mutable_payload());
            wave_tasks.push_back(std::move(spec));
        }
        waves_.push_back(std::move(wave_tasks));
    }
    total_files_ = files.size();
    return true;
}

bool SchedulerPlugin::plan(const common::RunRequest& request,
                            const std::vector<common::ClientInfo>& clients) {
    (void)clients;
    std::lock_guard<std::mutex> lock(mutex_);

    run_id_ = request.run_id;
    if (!BuildWaves(run_id_)) {
        return false;
    }
    pending_.clear();
    pending_ids_.clear();
    priority_pending_.clear();
    priority_ids_.clear();
    wave_outstanding_.clear();
    task_wave_.clear();
    settled_waves_.clear();
    task_client_.clear();
    tasks_by_id_.clear();
    cumulative_.clear();
    {
        std::lock_guard<std::mutex> settle_lock(settle_mutex_);
        settle_queue_.clear();
        settle_failed_wave_.store(-1);
        settle_active_.store(false);
    }
    selected_count_.store(0);

    // All waves are dispatchable from the start; waves only define
    // settlement batches. The pool keeps tasks in wave order.
    for (size_t wave = 0; wave < waves_.size(); ++wave) {
        for (const common::Task& task : waves_[wave]) {
            pending_.push_back(task);
            pending_ids_.insert(task.task_id());
            wave_outstanding_[static_cast<int>(wave)].insert(task.task_id());
            task_wave_[task.task_id()] = static_cast<int>(wave);
            tasks_by_id_[task.task_id()] = task;
        }
    }
    SaveCheckpointLocked();
    if (!last_checkpoint_save_ok_) {
        LOG_ERROR("coverage plugin: cannot persist initial checkpoint, run not started");
        return false;
    }
    LOG_INFO("coverage plugin: planned " << total_files_ << " files, " << waves_.size()
                                         << " waves, " << pending_.size()
                                         << " tasks ready to dispatch");
    return true;
}

bool SchedulerPlugin::requestTask(const common::ClientInfo& client,
                                  const std::string& request_payload, common::Task& task) {
    (void)request_payload;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!priority_pending_.empty()) {
        task = priority_pending_.front();
        priority_pending_.erase(priority_pending_.begin());
        priority_ids_.erase(task.task_id());
        task_client_[task.task_id()] = client.identity;
        return true;
    }
    if (pending_.empty()) {
        return false;
    }
    task = pending_.front();
    pending_.erase(pending_.begin());
    pending_ids_.erase(task.task_id());
    task_client_[task.task_id()] = client.identity;
    return true;
}

void SchedulerPlugin::requeue(const std::vector<std::string>& task_ids) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const std::string& task_id : task_ids) {
        task_client_.erase(task_id);
        const auto spec_it = tasks_by_id_.find(task_id);
        if (spec_it == tasks_by_id_.end()) {
            LOG_WARN("coverage plugin: cannot requeue unknown task " << task_id);
            continue;
        }
        const auto wave_it = task_wave_.find(task_id);
        if (wave_it == task_wave_.end() || settled_waves_.count(wave_it->second) > 0) {
            LOG_WARN("coverage plugin: cannot requeue task of settled wave " << task_id);
            continue;
        }
        if (priority_ids_.count(task_id) == 0 && pending_ids_.count(task_id) == 0) {
            priority_pending_.push_back(spec_it->second);
            priority_ids_.insert(task_id);
            wave_outstanding_[wave_it->second].insert(task_id);
            LOG_WARN("coverage plugin: requeued to priority " << task_id);
        } else {
            LOG_WARN("coverage plugin: task already queued, skip requeue " << task_id);
        }
    }
}

void SchedulerPlugin::SaveCheckpointLocked() {
    if (run_id_.empty()) {
        last_checkpoint_save_ok_ = false;
        return;
    }
    coverage::CoverageCheckpoint ckpt;
    ckpt.set_checkpoint_version(2);
    // Legacy field: first unsettled wave index (settled waves are always a
    // prefix when the chain has not failed).
    ckpt.set_current_wave(static_cast<std::int32_t>(settled_waves_.size()));
    ckpt.set_total_files(static_cast<std::int32_t>(total_files_));
    ckpt.set_selected_files(static_cast<std::int32_t>(selected_count_.load()));
    ckpt.set_manifest_fingerprint(manifest_fingerprint_);
    ckpt.set_task_seq(0);
    for (const auto& wave_item : wave_outstanding_) {
        for (const std::string& task_id : wave_item.second) {
            if (priority_ids_.count(task_id) > 0) {
                continue;
            }
            const auto it = tasks_by_id_.find(task_id);
            if (it == tasks_by_id_.end()) {
                continue;
            }
            std::string bytes;
            it->second.SerializeToString(&bytes);
            ckpt.add_remaining_tasks(bytes);
        }
    }
    for (const std::string& task_id : priority_ids_) {
        const auto it = tasks_by_id_.find(task_id);
        if (it == tasks_by_id_.end()) {
            continue;
        }
        std::string bytes;
        it->second.SerializeToString(&bytes);
        ckpt.add_priority_tasks(bytes);
    }
    for (int wave : settled_waves_) {
        ckpt.add_settled_waves(wave);
    }
    {
        // Snapshot settlement state under settle_mutex_; the merge itself
        // never holds this lock, so result bookkeeping is never blocked.
        std::lock_guard<std::mutex> settle_lock(settle_mutex_);
        for (int wave : settle_queue_) {
            ckpt.add_pending_settle_waves(wave);
        }
        ckpt.set_settle_failed_wave(settle_failed_wave_.load());
    }
    std::string blob;
    if (!ckpt.SerializeToString(&blob)) {
        last_checkpoint_save_ok_ = false;
        LOG_ERROR("coverage plugin: failed to serialize checkpoint for run " << run_id_);
        return;
    }
    last_checkpoint_save_ok_ = true;
    if (!store_->saveCheckpoint(run_id_, blob)) {
        LOG_ERROR("coverage plugin: failed to save checkpoint for run " << run_id_
                                                                        << " (checkpoint gap)");
        last_checkpoint_save_ok_ = false;
        return;
    }
}

bool SchedulerPlugin::saveCheckpoint(const std::string& run_id) {
    // Wait for any in-flight settlement so the snapshot is stable (stop/quit).
    while (true) {
        {
            std::unique_lock<std::mutex> settle_lock(settle_mutex_);
            settle_cv_.wait(settle_lock, [this] {
                return !settle_active_.load(std::memory_order_relaxed);
            });
        }
        bool active = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            {
                std::lock_guard<std::mutex> settle_lock(settle_mutex_);
                active = settle_active_.load(std::memory_order_relaxed);
            }
            if (!active) {
                if (run_id_ != run_id) {
                    LOG_WARN("coverage plugin: saveCheckpoint run mismatch, current=" << run_id_
                                                                                      << " asked=" << run_id);
                    return false;
                }
                SaveCheckpointLocked();
                return last_checkpoint_save_ok_;
            }
        }
        // A new settlement started after the wait; retry with locks released.
    }
}

bool SchedulerPlugin::restore(const std::string& run_id) {
    bool resume_settle = false;
    size_t pending_settle_count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string blob;
        if (!store_->loadCheckpoint(run_id, &blob)) {
            LOG_WARN("coverage plugin: no checkpoint for run " << run_id);
            return false;
        }
        coverage::CoverageCheckpoint ckpt;
        if (!ckpt.ParseFromString(blob)) {
            LOG_ERROR("coverage plugin: unparseable checkpoint for run " << run_id);
            return false;
        }
        if (!BuildWaves(run_id)) {
            LOG_ERROR("coverage plugin: restore failed to rebuild waves for run " << run_id);
            return false;
        }
        if (!ckpt.manifest_fingerprint().empty() &&
            ckpt.manifest_fingerprint() != manifest_fingerprint_) {
            LOG_ERROR("coverage plugin: manifest changed since checkpoint, cannot restore run "
                      << run_id);
            return false;
        }
        if (ckpt.manifest_fingerprint().empty()) {
            LOG_WARN("coverage plugin: legacy checkpoint without manifest fingerprint for run "
                     << run_id);
        }

        run_id_ = run_id;
        pending_.clear();
        pending_ids_.clear();
        priority_pending_.clear();
        priority_ids_.clear();
        wave_outstanding_.clear();
        task_wave_.clear();
        settled_waves_.clear();
        task_client_.clear();
        tasks_by_id_.clear();
        cumulative_.clear();
        total_files_ = static_cast<size_t>(ckpt.total_files());
        selected_count_.store(static_cast<size_t>(ckpt.selected_files()));

        const auto wave_of = [](const common::Task& task) {
            coverage::CoverageTask meta;
            return meta.ParseFromString(task.payload()) ? meta.wave_id() : 0;
        };

        size_t legacy_wave = 0;
        const bool multi_wave = ckpt.checkpoint_version() >= 2;
        if (multi_wave) {
            for (int wave : ckpt.settled_waves()) {
                settled_waves_.insert(wave);
            }
        } else {
            // Legacy single-wave checkpoint: waves before current_wave are
            // settled; later waves were never dispatched and go back to the pool.
            legacy_wave = static_cast<size_t>(ckpt.current_wave());
            for (size_t wave = 0; wave < legacy_wave; ++wave) {
                settled_waves_.insert(static_cast<int>(wave));
            }
        }

        const auto add_task = [&](const std::string& bytes, bool priority) {
            common::Task task;
            if (!task.ParseFromString(bytes)) {
                LOG_ERROR("coverage plugin: unparseable task in checkpoint");
                return false;
            }
            const int wave = wave_of(task);
            wave_outstanding_[wave].insert(task.task_id());
            task_wave_[task.task_id()] = wave;
            tasks_by_id_[task.task_id()] = task;
            if (priority) {
                priority_pending_.push_back(task);
                priority_ids_.insert(task.task_id());
            } else {
                pending_.push_back(task);
                pending_ids_.insert(task.task_id());
            }
            return true;
        };
        for (const std::string& bytes : ckpt.remaining_tasks()) {
            if (!add_task(bytes, false)) {
                return false;
            }
        }
        for (const std::string& bytes : ckpt.priority_tasks()) {
            if (!add_task(bytes, true)) {
                return false;
            }
        }

        if (!multi_wave) {
            // Waves after the legacy current wave were not dispatched yet.
            for (size_t wave = legacy_wave + 1; wave < waves_.size(); ++wave) {
                for (const common::Task& task : waves_[wave]) {
                    pending_.push_back(task);
                    pending_ids_.insert(task.task_id());
                    wave_outstanding_[static_cast<int>(wave)].insert(task.task_id());
                    task_wave_[task.task_id()] = static_cast<int>(wave);
                    tasks_by_id_[task.task_id()] = task;
                }
            }
        }

        if (!keep_staging_) {
            // Remove staging only for waves already settled.
            for (int wave : settled_waves_) {
                store_->clearWave(run_id, wave);
            }
        }

        if (!settled_waves_.empty()) {
            const int last_settled = *std::max_element(settled_waves_.begin(),
                                                       settled_waves_.end());
            std::vector<std::uint64_t> points;
            if (!store_->loadCumulative(run_id, last_settled, &points)) {
                LOG_ERROR("coverage plugin: cannot load cumulative points for run " << run_id);
                return false;
            }
            cumulative_.insert(points.begin(), points.end());
        }

        // Restore settlement state; pending waves are re-settled after the
        // plugin lock is released (no concurrent threads exist yet at start).
        {
            std::lock_guard<std::mutex> settle_lock(settle_mutex_);
            settle_queue_.clear();
            if (ckpt.has_settle_failed_wave()) {
                settle_failed_wave_.store(ckpt.settle_failed_wave());
            } else {
                settle_failed_wave_.store(-1);
            }
            for (int wave : ckpt.pending_settle_waves()) {
                settle_queue_.push_back(wave);
            }
            if (settle_failed_wave_.load() < 0 && !settle_queue_.empty()) {
                settle_active_.store(true);
                resume_settle = true;
            }
            pending_settle_count = settle_queue_.size();
        }
        LOG_INFO("coverage plugin: run restored " << run_id << " settled=" << settled_waves_.size()
                                                  << " outstanding=" << wave_outstanding_.size()
                                                  << " pending_settle=" << pending_settle_count);
    }

    if (settle_failed_wave_.load() >= 0) {
        LOG_ERROR("coverage plugin: run " << run_id << " has settlement failure at wave "
                  << settle_failed_wave_.load() << ", settlement chain stays paused");
    }
    if (resume_settle) {
        LOG_INFO("coverage plugin: resuming settlement for run " << run_id);
        SettleWaveLoop();
    }
    return true;
}

void SchedulerPlugin::processResult(const common::TaskResult& result) {
    if (HandleResultLocked(result)) {
        SettleWaveLoop();
    }
}

bool SchedulerPlugin::HandleResultLocked(const common::TaskResult& result) {
    std::lock_guard<std::mutex> lock(mutex_);

    coverage::CoverageResult coverage_result;
    int covered_lines = 0;
    if (!coverage_result.ParseFromString(result.result())) {
        LOG_ERROR("coverage plugin: unparseable result for " << result.task_id());
    } else {
        for (const coverage::FileCoverage& file : coverage_result.files()) {
            covered_lines += file.covered_lines();
            if (!file.ok()) {
                LOG_WARN("coverage plugin: file failed: " << file.file());
            }
        }
    }

    const auto tw_it = task_wave_.find(result.task_id());
    if (tw_it == task_wave_.end()) {
        LOG_WARN("coverage plugin: result for unknown task " << result.task_id()
                                                             << " after run state change, dropped");
        return false;
    }
    const int wave = tw_it->second;
    if (settled_waves_.count(wave) > 0) {
        LOG_WARN("coverage plugin: late result for settled wave " << wave << ", task "
                  << result.task_id() << " dropped");
        return false;
    }
    wave_outstanding_[wave].erase(result.task_id());
    task_client_.erase(result.task_id());

    LOG_INFO("coverage plugin: result " << result.task_id() << " covered_lines="
                                        << covered_lines);

    if (!wave_outstanding_[wave].empty()) {
        SaveCheckpointLocked();
        return false;
    }

    // Wave ready to settle; dispatch of other waves is unaffected.
    bool should_run = false;
    {
        std::lock_guard<std::mutex> settle_lock(settle_mutex_);
        if (settle_failed_wave_.load() < 0 && settled_waves_.count(wave) == 0) {
            if (std::find(settle_queue_.begin(), settle_queue_.end(), wave) ==
                settle_queue_.end()) {
                settle_queue_.push_back(wave);
            }
            if (!settle_active_.load()) {
                settle_active_.store(true);
                should_run = true;
            }
        }
    }
    SaveCheckpointLocked();
    return should_run;
}

void SchedulerPlugin::SettleWaveLoop() {
    while (true) {
        int wave = -1;
        {
            std::lock_guard<std::mutex> settle_lock(settle_mutex_);
            if (settle_failed_wave_.load() >= 0 || settle_queue_.empty()) {
                settle_active_.store(false);
                settle_cv_.notify_all();
                return;
            }
            wave = settle_queue_.front();
            settle_queue_.pop_front();
        }

        const bool ok = SettleWave(wave);
        if (!ok) {
            LOG_ERROR("coverage plugin: wave " << wave
                      << " settlement failed, chain paused (tasks keep dispatching)");
            {
                std::lock_guard<std::mutex> settle_lock(settle_mutex_);
                settle_failed_wave_.store(wave);
                settle_queue_.clear();
                settle_active_.store(false);
            }
            settle_cv_.notify_all();
            std::lock_guard<std::mutex> lock(mutex_);
            SaveCheckpointLocked();
            return;
        }
        if (!keep_staging_) {
            if (!store_->clearWave(run_id_, wave)) {
                LOG_ERROR("coverage plugin: clearWave failed for wave " << wave);
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        settled_waves_.insert(wave);
        SaveCheckpointLocked();
    }
}

bool SchedulerPlugin::runFinished() {
    std::lock_guard<std::mutex> lock(mutex_);
    return waves_.empty() || settled_waves_.size() >= waves_.size();
}

void SchedulerPlugin::cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    waves_.clear();
    pending_.clear();
    pending_ids_.clear();
    priority_pending_.clear();
    priority_ids_.clear();
    wave_outstanding_.clear();
    task_wave_.clear();
    settled_waves_.clear();
    task_client_.clear();
    tasks_by_id_.clear();
    cumulative_.clear();
    total_files_ = 0;
    selected_count_.store(0);
    manifest_fingerprint_.clear();
    run_id_.clear();
    last_checkpoint_save_ok_ = false;
    {
        std::lock_guard<std::mutex> settle_lock(settle_mutex_);
        settle_queue_.clear();
        settle_failed_wave_.store(-1);
        settle_active_.store(false);
    }
    settle_cv_.notify_all();
    LOG_INFO("coverage plugin: run cancelled, task pool cleared");
}

bool SchedulerPlugin::SettleWave(int wave) {
    // Runs on the single settlement loop; no plugin mutex is held so result
    // bookkeeping and dispatch stay responsive during the heavy merge.
    std::unordered_map<std::string, std::set<std::uint64_t>> file_points;
    if (!store_->loadWave(run_id_, wave, &file_points)) {
        LOG_ERROR("coverage plugin: loadWave failed for wave " << wave);
        return false;
    }

    // Batch greedy: repeatedly pick the file adding the most NEW points.
    std::vector<std::string> selected;
    std::unordered_set<std::string> taken;
    while (true) {
        std::string best;
        size_t best_new = 0;
        for (const auto& item : file_points) {
            if (taken.count(item.first)) {
                continue;
            }
            size_t new_count = 0;
            for (std::uint64_t point : item.second) {
                if (!cumulative_.count(point)) {
                    ++new_count;
                }
            }
            if (new_count > best_new) {
                best_new = new_count;
                best = item.first;
            }
        }
        if (best.empty() || best_new == 0) {
            break;
        }
        selected.push_back(best);
        taken.insert(best);
        cumulative_.insert(file_points[best].begin(), file_points[best].end());
    }
    if (!store_->appendSelected(run_id_, selected)) {
        LOG_ERROR("coverage plugin: appendSelected failed for wave " << wave);
        return false;
    }
    std::vector<std::uint64_t> cumulative_vec(cumulative_.begin(), cumulative_.end());
    if (!store_->saveCumulative(run_id_, wave, cumulative_vec)) {
        LOG_ERROR("coverage plugin: saveCumulative failed for wave " << wave);
        return false;
    }
    const size_t new_selected_count = selected_count_.load() + selected.size();
    if (!store_->saveRunSummary(run_id_, static_cast<int>(total_files_),
                                static_cast<int>(new_selected_count),
                                static_cast<std::int64_t>(cumulative_.size()))) {
        LOG_ERROR("coverage plugin: saveRunSummary failed for wave " << wave);
        return false;
    }
    if (store_cov_ && !MergeWaveCoverage(wave)) {
        LOG_ERROR("coverage plugin: wave " << wave << " cov merge failed");
        return false;
    }
    selected_count_.store(new_selected_count);
    LOG_INFO("coverage plugin: wave " << wave << " settled, selected "
                                      << selected.size() << "/" << file_points.size()
                                      << " files, cumulative points=" << cumulative_.size());
    return true;
}

bool SchedulerPlugin::MergeWaveCoverage(int wave) {
#ifdef _WIN32
    const std::filesystem::path work_dir =
        std::filesystem::temp_directory_path() /
        ("tt-cov-" + run_id_ + "-w" + std::to_string(wave));
    std::error_code ec;
    std::filesystem::remove_all(work_dir, ec);
    std::filesystem::create_directories(work_dir, ec);
    if (ec) {
        LOG_ERROR("coverage plugin: cannot create cov work dir: " << ec.message());
        return false;
    }

    // Keep every OpenCppCoverage invocation bounded to two input artifacts.
    // A whole wave can contain thousands of files, which otherwise exceeds
    // Windows' command-line limit before OpenCppCoverage gets a chance to run.
    const auto cleanup = [&]() {
        std::error_code cleanup_ec;
        std::filesystem::remove_all(work_dir, cleanup_ec);
        if (cleanup_ec) {
            LOG_WARN("coverage plugin: cannot clean cov work dir " << work_dir << ": "
                                                                   << cleanup_ec.message());
        }
    };
    const auto fail = [&]() {
        cleanup();
        return false;
    };
    const auto quote = [](const std::string& value) {
        std::string result = "\"";
        for (const char ch : value) {
            if (ch == '\"') {
                result += "\\\"";
            } else {
                result += ch;
            }
        }
        result += '\"';
        return result;
    };
    const auto build_command = [&](const std::vector<std::string>& inputs,
                                   const std::string& output_cov,
                                   const std::string* output_xml) {
        std::string command = quote(opencppcoverage_) + " --quiet";
        for (const std::string& input : inputs) {
            command += " --input_coverage=" + quote(input);
        }
        command += " --export_type=binary:" + quote(output_cov);
        if (output_xml != nullptr) {
            command += " --export_type=cobertura:" + quote(*output_xml);
        }
        return command;
    };

    std::vector<std::string> wave_covs;
    if (!store_->exportWaveCovs(run_id_, wave, work_dir.string(), wave_covs)) {
        LOG_ERROR("coverage plugin: exportWaveCovs failed for wave " << wave);
        return fail();
    }
    std::string cumulative_path;
    if (!store_->exportCumulativeCov(run_id_, work_dir.string(), cumulative_path)) {
        LOG_ERROR("coverage plugin: exportCumulativeCov failed for wave " << wave);
        return fail();
    }
    if (wave_covs.empty() && cumulative_path.empty()) {
        LOG_ERROR("coverage plugin: wave " << wave << " has no cov artifacts to merge");
        return fail();
    }

    std::vector<std::string> inputs;
    if (!cumulative_path.empty()) {
        inputs.push_back(cumulative_path);
    }
    std::sort(wave_covs.begin(), wave_covs.end());
    inputs.insert(inputs.end(), wave_covs.begin(), wave_covs.end());

    // Reduce the inputs as a binary tree. Odd artifacts are carried to the
    // next round unchanged; the final invocation emits both the cumulative
    // binary and the cobertura report used for the wave summary.
    int round = 0;
    while (inputs.size() > 2) {
        std::vector<std::string> next_round;
        next_round.reserve((inputs.size() + 1) / 2);
        for (size_t i = 0; i < inputs.size(); i += 2) {
            if (i + 1 == inputs.size()) {
                next_round.push_back(inputs[i]);
                continue;
            }
            const std::string merged_path =
                (work_dir / ("merge-r" + std::to_string(round) + "-p" +
                             std::to_string(i / 2) + ".cov"))
                    .string();
            const std::vector<std::string> pair = {inputs[i], inputs[i + 1]};
            const int exit_code = RunCommand(build_command(pair, merged_path, nullptr));
            if (exit_code != 0) {
                LOG_ERROR("coverage plugin: intermediate cov merge failed for wave " << wave
                                                                                       << " round="
                                                                                       << round
                                                                                       << " pair="
                                                                                       << (i / 2)
                                                                                       << " exit="
                                                                                       << exit_code);
                return fail();
            }
            std::error_code exists_ec;
            if (!std::filesystem::is_regular_file(merged_path, exists_ec) || exists_ec) {
                LOG_ERROR("coverage plugin: intermediate cov merge produced no file: "
                          << merged_path);
                return fail();
            }
            next_round.push_back(merged_path);
        }
        inputs.swap(next_round);
        ++round;
    }

    const std::string out_cov = (work_dir / "cumulative.cov").string();
    const std::string out_xml = (work_dir / "summary.xml").string();
    const int exit_code = RunCommand(build_command(inputs, out_cov, &out_xml));
    if (exit_code != 0) {
        LOG_ERROR("coverage plugin: cov merge failed for wave " << wave
                                                               << " exit=" << exit_code);
        return fail();
    }
    std::int64_t covered = 0;
    std::int64_t total = 0;
    if (!ParseCoverageTotals(out_xml, &covered, &total)) {
        LOG_ERROR("coverage plugin: cannot parse merged cobertura for wave " << wave);
        return fail();
    }
    if (!store_->saveCumulativeCov(run_id_, wave, out_cov)) {
        LOG_ERROR("coverage plugin: saveCumulativeCov failed for wave " << wave);
        return fail();
    }
    const double percent = total > 0 ? static_cast<double>(covered) / total : 0.0;
    if (!store_->saveWaveSummary(run_id_, wave, covered, total, percent)) {
        LOG_ERROR("coverage plugin: saveWaveSummary failed for wave " << wave);
        return fail();
    }
    cleanup();
    LOG_INFO("coverage plugin: wave " << wave << " total coverage=" << covered << "/"
                                      << total << " (" << (percent * 100.0) << "%)");
    return true;
#else
    LOG_ERROR("coverage plugin: cov merge requires OpenCppCoverage (Windows only)");
    return false;
#endif
}

std::string SchedulerPlugin::ManifestFingerprint() const {
    std::ifstream in(manifest_path_, std::ios::binary);
    if (!in.is_open()) {
        return std::string();
    }
    std::ostringstream content;
    content << in.rdbuf();
    const std::string data = content.str();
    return std::to_string(data.size()) + ":" + std::to_string(Fnv1a64(data));
}

}  // namespace coverage_plugin
