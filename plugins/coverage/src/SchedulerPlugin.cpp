#include "coverage/SchedulerPlugin.h"

#include "common/Logger.h"

#include "coverage.pb.h"

#include <json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <utility>

namespace coverage_plugin {

std::string SchedulerPlugin::taskType() const {
    return "coverage";
}

bool SchedulerPlugin::init(const std::string& config) {
    const nlohmann::json cfg = nlohmann::json::parse(config);
    manifest_path_ = cfg.value("stp_manifest", std::string());
    batch_size_ = cfg.value("batch_size", 1000);
    task_size_ = cfg.value("task_size", 25);
    keep_staging_ = cfg.value("keep_staging", false);
    if (manifest_path_.empty()) {
        LOG_ERROR("coverage plugin: stp_manifest is required");
        return false;
    }
    store_ = CreateCoverageStore();
    if (!store_->init(config)) {
        LOG_ERROR("coverage plugin: coverage store init failed");
        return false;
    }
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
    wave_task_ids_.clear();
    task_client_.clear();
    client_tasks_.clear();
    tasks_by_id_.clear();
    cumulative_.clear();
    current_wave_ = 0;
    total_files_ = files.size();
    selected_count_ = 0;

    for (size_t start = 0; start < files.size(); start += static_cast<size_t>(batch_size_)) {
        const size_t end = std::min(start + static_cast<size_t>(batch_size_), files.size());
        std::vector<common::TaskSpec> wave_tasks;
        const size_t wave_index = waves_.size();
        for (size_t t = start; t < end; t += static_cast<size_t>(task_size_)) {
            const size_t tend = std::min(t + static_cast<size_t>(task_size_), end);
            coverage::CoverageTask task;
            task.set_run_id(request.run_id);
            task.set_wave_id(static_cast<std::int32_t>(wave_index));
            for (size_t i = t; i < tend; ++i) {
                task.add_files(files[i].first);
            }

            common::TaskSpec spec;
            spec.set_task_id(run_id_ + "-w" + std::to_string(wave_index) + "-t" +
                             std::to_string(wave_tasks.size()));
            task.SerializeToString(spec.mutable_payload());
            wave_tasks.push_back(std::move(spec));
        }
        waves_.push_back(std::move(wave_tasks));
    }

    pending_ = waves_[0];
    for (const common::TaskSpec& task : pending_) {
        wave_task_ids_.insert(task.task_id());
        tasks_by_id_[task.task_id()] = task;
    }
    LOG_INFO("coverage plugin: planned " << total_files_ << " files, " << waves_.size()
                                         << " waves, " << pending_.size()
                                         << " tasks in first wave");
    return true;
}

bool SchedulerPlugin::dispatch(const std::vector<common::ClientInfo>& clients,
                               std::vector<common::TaskAssignment>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const common::ClientInfo& client : clients) {
        if (pending_.empty()) {
            break;
        }
        if (client.heartbeat.status() != common::Heartbeat::IDLE) {
            continue;
        }
        if (client_tasks_[client.identity] > 0) {
            continue;  // one task at a time per client
        }

        common::TaskAssignment assignment;
        assignment.client_identity = client.identity;
        assignment.task = pending_.front();
        pending_.erase(pending_.begin());
        task_client_[assignment.task.task_id()] = client.identity;
        ++client_tasks_[client.identity];
        out.push_back(std::move(assignment));
    }
    return true;
}

void SchedulerPlugin::requeue(const std::vector<std::string>& task_ids) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const std::string& task_id : task_ids) {
        const auto it = task_client_.find(task_id);
        if (it != task_client_.end()) {
            --client_tasks_[it->second];
            task_client_.erase(it);
        }
        const auto spec_it = tasks_by_id_.find(task_id);
        if (spec_it != tasks_by_id_.end()) {
            pending_.push_back(spec_it->second);
        }
        LOG_WARN("coverage plugin: requeued " << task_id);
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
    LOG_INFO("coverage plugin: result " << result.task_id() << " covered_lines="
                                        << covered_lines);

    const auto task_it = task_client_.find(result.task_id());
    if (task_it != task_client_.end()) {
        --client_tasks_[task_it->second];
        task_client_.erase(task_it);
    }

    if (wave_task_ids_.empty()) {
        SettleWave();
    }
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
        for (const common::TaskSpec& task : pending_) {
            wave_task_ids_.insert(task.task_id());
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
