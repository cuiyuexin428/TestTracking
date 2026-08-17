#include "coverage/SchedulerPlugin.h"

#include "common/Logger.h"

#include "coverage.pb.h"

#include <json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
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
    shared_dir_ = cfg.value("shared_dir", std::string());
    batch_size_ = cfg.value("batch_size", 1000);
    task_size_ = cfg.value("task_size", 25);
    keep_staging_ = cfg.value("keep_staging", false);
    if (manifest_path_.empty()) {
        LOG_ERROR("coverage plugin: stp_manifest is required");
        return false;
    }
    store_ = CreateCoverageStore(config);
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
    if (info.deployed() && version_ok) {
        result.accepted = true;
        result.reason = "ok";
        return true;
    }

    coverage::CoverageValidationReply reply;
    reply.set_deploy_required(true);
    reply.set_target_program_version(expected_program_version_);
    reply.set_target_dataset_version(expected_dataset_version_);
    reply.set_shared_dir(shared_dir_);
    reply.SerializeToString(&result.reply_payload);
    result.accepted = false;
    result.reason = "deploy required";
    return true;
}

bool SchedulerPlugin::plan(const common::RunRequest& request,
                           const std::vector<common::ClientInfo>& clients) {
    (void)clients;
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::pair<std::string, std::uint64_t>> files;
    std::ifstream in(manifest_path_);
    if (!in.is_open()) {
        LOG_ERROR("coverage plugin: cannot open manifest " << manifest_path_);
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
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
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });

    run_id_ = request.run_id;
    waves_.clear();
    pending_.clear();
    pending_ids_.clear();
    priority_pending_.clear();
    priority_ids_.clear();
    wave_task_ids_.clear();
    task_client_.clear();
    tasks_by_id_.clear();
    cumulative_.clear();
    current_wave_ = 0;
    total_files_ = files.size();
    selected_count_ = 0;

    for (size_t start = 0; start < files.size(); start += static_cast<size_t>(batch_size_)) {
        const size_t end = std::min(start + static_cast<size_t>(batch_size_), files.size());
        std::vector<common::Task> wave_tasks;
        const size_t wave_index = waves_.size();
        for (size_t t = start; t < end; t += static_cast<size_t>(task_size_)) {
            const size_t tend = std::min(t + static_cast<size_t>(task_size_), end);
            coverage::CoverageTask task;
            task.set_run_id(request.run_id);
            task.set_wave_id(static_cast<std::int32_t>(wave_index));
            for (size_t i = t; i < tend; ++i) {
                task.add_files(files[i].first);
            }

            common::Task spec;
            spec.set_task_id(run_id_ + "-w" + std::to_string(wave_index) + "-t" +
                             std::to_string(wave_tasks.size()));
            task.SerializeToString(spec.mutable_payload());
            wave_tasks.push_back(std::move(spec));
        }
        waves_.push_back(std::move(wave_tasks));
    }

    pending_ = waves_[0];
    for (const common::Task& task : pending_) {
        wave_task_ids_.insert(task.task_id());
        pending_ids_.insert(task.task_id());
        tasks_by_id_[task.task_id()] = task;
    }
    LOG_INFO("coverage plugin: planned " << total_files_ << " files, " << waves_.size()
                                         << " waves, " << pending_.size()
                                         << " tasks in first wave");
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
        if (priority_ids_.count(task_id) == 0 && pending_ids_.count(task_id) == 0) {
            priority_pending_.push_back(spec_it->second);
            priority_ids_.insert(task_id);
            LOG_WARN("coverage plugin: requeued to priority " << task_id);
        } else {
            LOG_WARN("coverage plugin: task already queued, skip requeue " << task_id);
        }
    }
}

void SchedulerPlugin::processResult(const common::TaskResult& result) {
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

    std::lock_guard<std::mutex> lock(mutex_);
    wave_task_ids_.erase(result.task_id());
    task_client_.erase(result.task_id());
    LOG_INFO("coverage plugin: result " << result.task_id() << " covered_lines="
                                        << covered_lines);

    if (wave_task_ids_.empty()) {
        SettleWave();
    }
}

bool SchedulerPlugin::runFinished() {
    std::lock_guard<std::mutex> lock(mutex_);
    return waves_.empty() || current_wave_ >= waves_.size();
}

void SchedulerPlugin::cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    waves_.clear();
    pending_.clear();
    pending_ids_.clear();
    priority_pending_.clear();
    priority_ids_.clear();
    wave_task_ids_.clear();
    task_client_.clear();
    tasks_by_id_.clear();
    cumulative_.clear();
    current_wave_ = 0;
    total_files_ = 0;
    selected_count_ = 0;
    LOG_INFO("coverage plugin: run cancelled, task pool cleared");
}

void SchedulerPlugin::SettleWave() {
    auto file_points = store_->loadWave(run_id_, static_cast<int>(current_wave_));

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
    selected_count_ += selected.size();

    store_->appendSelected(run_id_, selected);
    std::vector<std::uint64_t> cumulative_vec(cumulative_.begin(), cumulative_.end());
    store_->saveCumulative(run_id_, static_cast<int>(current_wave_), cumulative_vec);
    store_->saveRunSummary(run_id_, static_cast<int>(total_files_),
                           static_cast<int>(selected_count_),
                           static_cast<std::int64_t>(cumulative_.size()));
    if (!keep_staging_) {
        store_->clearWave(run_id_, static_cast<int>(current_wave_));
    }
    LOG_INFO("coverage plugin: wave " << current_wave_ << " settled, selected "
                                      << selected.size() << "/" << file_points.size()
                                      << " files, cumulative points=" << cumulative_.size());

    ++current_wave_;
    if (current_wave_ < waves_.size()) {
        pending_ = waves_[current_wave_];
        wave_task_ids_.clear();
        pending_ids_.clear();
        for (const common::Task& task : pending_) {
            wave_task_ids_.insert(task.task_id());
            pending_ids_.insert(task.task_id());
        }
        LOG_INFO("coverage plugin: wave " << current_wave_ << " started, " << pending_.size()
                                          << " tasks");
    } else {
        LOG_INFO("coverage plugin: run finished, selected " << selected_count_ << "/"
                                                            << total_files_
                                                            << " files, cumulative points="
                                                            << cumulative_.size());
    }
}

}  // namespace coverage_plugin
