#include "coverage/SchedulerPlugin.h"

#include "common/Logger.h"

#include "coverage.pb.h"
#include "coverage/CoverageUtil.h"

#include <json.hpp>

#include <algorithm>
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
    task_size_ = cfg.value("task_size", 25);
    if (manifest_path_.empty()) {
        LOG_ERROR("coverage plugin: stp_manifest is required");
        return false;
    }
    if (task_size_ <= 0) {
        LOG_ERROR("coverage plugin: task_size must be > 0");
        return false;
    }
    store_ = CreateCoverageCheckpointStore(config);
    if (!store_->init(config)) {
        LOG_ERROR("coverage plugin: coverage store init failed");
        return false;
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

bool SchedulerPlugin::BuildTasks(const std::string& run_id) {
    all_files_.clear();
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
        all_files_.push_back(comma == std::string::npos ? line : line.substr(0, comma));
    }
    if (all_files_.empty()) {
        LOG_ERROR("coverage plugin: manifest is empty");
        return false;
    }
    manifest_fingerprint_ = ManifestFingerprint();
    if (manifest_fingerprint_.empty()) {
        LOG_ERROR("coverage plugin: cannot fingerprint manifest " << manifest_path_);
        return false;
    }

    pending_.clear();
    pending_ids_.clear();
    priority_pending_.clear();
    priority_ids_.clear();
    tasks_by_id_.clear();
    task_client_.clear();
    done_.clear();
    total_files_ = static_cast<int>(all_files_.size());
    total_tasks_ = 0;
    for (size_t start = 0; start < all_files_.size();
         start += static_cast<size_t>(task_size_)) {
        const size_t end = std::min(start + static_cast<size_t>(task_size_),
                                    all_files_.size());
        coverage::CoverageTask task;
        task.set_run_id(run_id);
        for (size_t i = start; i < end; ++i) {
            task.add_files(all_files_[i]);
        }
        common::Task spec;
        spec.set_task_id(run_id + "-t" + std::to_string(total_tasks_));
        task.SerializeToString(spec.mutable_payload());
        tasks_by_id_[spec.task_id()] = spec;
        pending_.push_back(spec);
        pending_ids_.insert(spec.task_id());
        ++total_tasks_;
    }
    return true;
}

bool SchedulerPlugin::plan(const common::RunRequest& request,
                            const std::vector<common::ClientInfo>& clients) {
    (void)clients;
    std::lock_guard<std::mutex> lock(mutex_);

    run_id_ = request.run_id;
    if (!BuildTasks(run_id_)) {
        return false;
    }
    SaveCheckpointLocked();
    if (!last_checkpoint_save_ok_) {
        LOG_ERROR("coverage plugin: cannot persist initial checkpoint, run not started");
        return false;
    }
    LOG_INFO("coverage plugin: planned " << total_files_ << " files, " << total_tasks_
                                         << " tasks ready to dispatch for run " << run_id_);
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
        if (done_.count(task_id) > 0) {
            LOG_WARN("coverage plugin: cannot requeue done task " << task_id);
            continue;
        }
        const auto spec_it = tasks_by_id_.find(task_id);
        if (spec_it == tasks_by_id_.end()) {
            LOG_WARN("coverage plugin: cannot requeue unknown task " << task_id);
            continue;
        }
        if (priority_ids_.count(task_id) == 0 && pending_ids_.count(task_id) == 0) {
            priority_pending_.push_back(spec_it->second);
            priority_ids_.insert(task_id);
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
    ckpt.set_checkpoint_version(3);
    ckpt.set_total_files(total_files_);
    ckpt.set_manifest_fingerprint(manifest_fingerprint_);
    // Persist every unfinished task: the pending backlog, reclaimed priority
    // tasks, and anything currently in flight (task_client_). On restore all of
    // these are re-dispatched, so no task is lost on STOP or crash.
    auto append_task = [&](const common::Task& task) {
        std::string bytes;
        task.SerializeToString(&bytes);
        ckpt.add_remaining_tasks(bytes);
    };
    for (const common::Task& task : pending_) {
        append_task(task);
    }
    for (const common::Task& task : priority_pending_) {
        append_task(task);
    }
    for (const auto& task_id : task_client_) {
        const auto it = tasks_by_id_.find(task_id.first);
        if (it != tasks_by_id_.end()) {
            append_task(it->second);
        }
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
    }
}

bool SchedulerPlugin::saveCheckpoint(const std::string& run_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (run_id_ != run_id) {
        LOG_WARN("coverage plugin: saveCheckpoint run mismatch, current=" << run_id_
                                                                          << " asked="
                                                                          << run_id);
        return false;
    }
    SaveCheckpointLocked();
    return last_checkpoint_save_ok_;
}

bool SchedulerPlugin::restore(const std::string& run_id) {
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
    if (ckpt.checkpoint_version() != 3) {
        LOG_ERROR("coverage plugin: unsupported checkpoint version " << ckpt.checkpoint_version()
                                                                      << " for run " << run_id);
        return false;
    }
    if (!BuildTasks(run_id)) {
        LOG_ERROR("coverage plugin: restore failed to rebuild tasks for run " << run_id);
        return false;
    }
    if (!ckpt.manifest_fingerprint().empty() &&
        ckpt.manifest_fingerprint() != manifest_fingerprint_) {
        LOG_ERROR("coverage plugin: manifest changed since checkpoint, cannot restore run "
                  << run_id);
        return false;
    }

    run_id_ = run_id;
    // BuildTasks populated pending_ from every manifest file; replace it with
    // only the unfinished tasks persisted in the checkpoint.
    std::unordered_set<std::string> unfinished;
    pending_.clear();
    pending_ids_.clear();
    priority_pending_.clear();
    priority_ids_.clear();
    for (const std::string& bytes : ckpt.remaining_tasks()) {
        common::Task task;
        if (!task.ParseFromString(bytes)) {
            LOG_ERROR("coverage plugin: unparseable task in checkpoint");
            return false;
        }
        unfinished.insert(task.task_id());
        pending_.push_back(task);
        pending_ids_.insert(task.task_id());
        tasks_by_id_[task.task_id()] = task;
    }
    done_.clear();
    for (const auto& kv : tasks_by_id_) {
        if (unfinished.count(kv.first) == 0) {
            done_.insert(kv.first);
        }
    }
    total_tasks_ = static_cast<int>(tasks_by_id_.size());
    total_files_ = static_cast<int>(all_files_.size());
    LOG_INFO("coverage plugin: run restored " << run_id << " done=" << done_.size() << "/"
                                              << total_tasks_ << " pending=" << pending_.size());
    return true;
}

void SchedulerPlugin::processResult(const common::TaskResult& result) {
    std::lock_guard<std::mutex> lock(mutex_);
    coverage::CoverageResult coverage_result;
    int ok_files = 0;
    int fail_files = 0;
    int covered_lines = 0;
    if (!coverage_result.ParseFromString(result.result())) {
        LOG_ERROR("coverage plugin: unparseable result for " << result.task_id());
    } else {
        for (const coverage::FileCoverage& file : coverage_result.files()) {
            covered_lines += file.covered_lines();
            if (file.ok()) {
                ++ok_files;
            } else {
                ++fail_files;
                LOG_WARN("coverage plugin: file failed: " << file.file());
            }
        }
    }
    task_client_.erase(result.task_id());
    done_.insert(result.task_id());
    SaveCheckpointLocked();
    LOG_INFO("coverage plugin: result " << result.task_id() << " exit=" << result.exit_code()
                                        << " ok=" << ok_files << " fail=" << fail_files
                                        << " covered=" << covered_lines << " done="
                                        << done_.size() << "/" << total_tasks_);
}

bool SchedulerPlugin::runFinished() {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_tasks_ > 0 && done_.size() >= static_cast<size_t>(total_tasks_) &&
           pending_.empty() && task_client_.empty() && priority_pending_.empty();
}

void SchedulerPlugin::cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.clear();
    pending_ids_.clear();
    priority_pending_.clear();
    priority_ids_.clear();
    tasks_by_id_.clear();
    task_client_.clear();
    done_.clear();
    all_files_.clear();
    total_files_ = 0;
    total_tasks_ = 0;
    manifest_fingerprint_.clear();
    run_id_.clear();
    last_checkpoint_save_ok_ = false;
    LOG_INFO("coverage plugin: run cancelled, task pool cleared");
}

std::string SchedulerPlugin::ManifestFingerprint() const {
    std::ifstream in(manifest_path_, std::ios::binary);
    if (!in.is_open()) {
        return std::string();
    }
    std::ostringstream content;
    content << in.rdbuf();
    const std::string data = content.str();
    return std::to_string(data.size()) + ":" + std::to_string(common::Fnv1a64(data));
}

}  // namespace coverage_plugin
