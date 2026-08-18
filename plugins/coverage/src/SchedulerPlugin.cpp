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
    failed_file_retries_ = cfg.value("failed_file_retries", 0);
    store_cov_ = cfg.value("store_cov", false);
    opencppcoverage_ = cfg.value("opencppcoverage", std::string("OpenCppCoverage.exe"));
    keep_staging_ = cfg.value("keep_staging", false);
    if (manifest_path_.empty()) {
        LOG_ERROR("coverage plugin: stp_manifest is required");
        return false;
    }
    if (failed_file_retries_ < 0) {
        LOG_ERROR("coverage plugin: failed_file_retries must be >= 0");
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
    wave_task_ids_.clear();
    task_client_.clear();
    tasks_by_id_.clear();
    cumulative_.clear();
    current_wave_ = 0;
    task_seq_ = 0;
    selected_count_ = 0;

    pending_ = waves_[0];
    for (const common::Task& task : pending_) {
        wave_task_ids_.insert(task.task_id());
        pending_ids_.insert(task.task_id());
        tasks_by_id_[task.task_id()] = task;
    }
    SaveCheckpointLocked();
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

void SchedulerPlugin::SaveCheckpointLocked() {
    if (run_id_.empty()) {
        return;
    }
    if (current_wave_ < waves_.size() && wave_task_ids_.empty()) {
        // A live wave must always have outstanding tasks; an empty pool here
        // means settlement failed or is in progress. Persisting this state
        // would make the run unresumable, so keep the previous checkpoint.
        LOG_ERROR("coverage plugin: refusing to save checkpoint with empty task pool "
                  "for active wave " << current_wave_);
        return;
    }
    coverage::CoverageCheckpoint ckpt;
    ckpt.set_current_wave(static_cast<std::int32_t>(current_wave_));
    ckpt.set_total_files(static_cast<std::int32_t>(total_files_));
    ckpt.set_selected_files(static_cast<std::int32_t>(selected_count_));
    ckpt.set_manifest_fingerprint(manifest_fingerprint_);
    ckpt.set_task_seq(static_cast<std::int32_t>(task_seq_));
    for (const std::string& task_id : wave_task_ids_) {
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
    for (const std::string& task_id : priority_ids_) {
        const auto it = tasks_by_id_.find(task_id);
        if (it == tasks_by_id_.end()) {
            continue;
        }
        std::string bytes;
        it->second.SerializeToString(&bytes);
        ckpt.add_priority_tasks(bytes);
    }
    std::string blob;
    if (!ckpt.SerializeToString(&blob)) {
        LOG_ERROR("coverage plugin: failed to serialize checkpoint for run " << run_id_);
        return;
    }
    if (!store_->saveCheckpoint(run_id_, blob)) {
        LOG_ERROR("coverage plugin: failed to save checkpoint for run " << run_id_);
    }
}

bool SchedulerPlugin::saveCheckpoint(const std::string& run_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (run_id_ != run_id) {
        LOG_WARN("coverage plugin: saveCheckpoint run mismatch, current=" << run_id_
                                                                         << " asked=" << run_id);
        return false;
    }
    SaveCheckpointLocked();
    return true;
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
    wave_task_ids_.clear();
    task_client_.clear();
    tasks_by_id_.clear();
    cumulative_.clear();
    current_wave_ = static_cast<size_t>(ckpt.current_wave());
    total_files_ = static_cast<size_t>(ckpt.total_files());
    selected_count_ = static_cast<size_t>(ckpt.selected_files());
    task_seq_ = static_cast<size_t>(ckpt.task_seq());

    if (!keep_staging_) {
        // Waves before the checkpoint wave are settled; remove any staging
        // left by an interrupted settle/cleanup sequence.
        for (size_t wave = 0; wave < current_wave_; ++wave) {
            store_->clearWave(run_id, static_cast<int>(wave));
        }
    }

    if (current_wave_ >= waves_.size()) {
        LOG_INFO("coverage plugin: run " << run_id << " checkpoint marks run finished");
        return true;
    }

    for (const std::string& bytes : ckpt.remaining_tasks()) {
        common::Task task;
        if (!task.ParseFromString(bytes)) {
            LOG_ERROR("coverage plugin: unparseable remaining task in checkpoint");
            return false;
        }
        pending_.push_back(task);
        pending_ids_.insert(task.task_id());
        wave_task_ids_.insert(task.task_id());
        tasks_by_id_[task.task_id()] = task;
    }
    for (const std::string& bytes : ckpt.priority_tasks()) {
        common::Task task;
        if (!task.ParseFromString(bytes)) {
            LOG_ERROR("coverage plugin: unparseable priority task in checkpoint");
            return false;
        }
        priority_pending_.push_back(task);
        priority_ids_.insert(task.task_id());
        wave_task_ids_.insert(task.task_id());
        tasks_by_id_[task.task_id()] = task;
    }
    for (size_t wave = current_wave_ + 1; wave < waves_.size(); ++wave) {
        for (const common::Task& task : waves_[wave]) {
            tasks_by_id_[task.task_id()] = task;
        }
    }

    if (current_wave_ < waves_.size() && wave_task_ids_.empty()) {
        LOG_ERROR("coverage plugin: checkpoint for run " << run_id
                                                         << " has no remaining tasks in an "
                                                            "active wave, restore rejected");
        return false;
    }
    std::vector<std::uint64_t> points;
    if (!store_->loadCumulative(run_id, static_cast<int>(current_wave_) - 1, &points)) {
        LOG_ERROR("coverage plugin: cannot load cumulative points for run " << run_id);
        return false;
    }
    cumulative_.insert(points.begin(), points.end());
    LOG_INFO("coverage plugin: run restored " << run_id << " wave=" << current_wave_
                                              << " remaining=" << wave_task_ids_.size());
    return true;
}

void SchedulerPlugin::processResult(const common::TaskResult& result) {
    coverage::CoverageResult coverage_result;
    int covered_lines = 0;
    bool result_parse_failed = false;
    std::vector<std::string> failed_files;
    if (!coverage_result.ParseFromString(result.result())) {
        LOG_ERROR("coverage plugin: unparseable result for " << result.task_id());
        result_parse_failed = true;
    } else {
        for (const coverage::FileCoverage& file : coverage_result.files()) {
            covered_lines += file.covered_lines();
            if (!file.ok()) {
                LOG_WARN("coverage plugin: file failed: " << file.file());
                failed_files.push_back(file.file());
            }
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    int retry_count = 0;
    int retry_wave = -1;
    std::string retry_run_id;
    const auto spec_it = tasks_by_id_.find(result.task_id());
    if (spec_it != tasks_by_id_.end()) {
        coverage::CoverageTask task_meta;
        if (task_meta.ParseFromString(spec_it->second.payload())) {
            retry_count = task_meta.retry_count();
            retry_run_id = task_meta.run_id();
            retry_wave = task_meta.wave_id();
            if (result_parse_failed) {
                for (const std::string& file : task_meta.files()) {
                    failed_files.push_back(file);
                }
            }
        }
    }
    wave_task_ids_.erase(result.task_id());
    task_client_.erase(result.task_id());

    if (failed_file_retries_ > 0 && retry_count < failed_file_retries_ &&
        retry_wave == static_cast<int>(current_wave_) &&
        !failed_files.empty()) {
        coverage::CoverageTask retry_task;
        retry_task.set_run_id(retry_run_id);
        retry_task.set_wave_id(retry_wave);
        retry_task.set_retry_count(retry_count + 1);
        for (const std::string& file : failed_files) {
            retry_task.add_files(file);
        }
        common::Task spec;
        spec.set_task_id(run_id_ + "-w" + std::to_string(retry_wave) + "-r" +
                         std::to_string(++task_seq_));
        retry_task.SerializeToString(spec.mutable_payload());
        pending_.push_back(spec);
        pending_ids_.insert(spec.task_id());
        wave_task_ids_.insert(spec.task_id());
        tasks_by_id_[spec.task_id()] = spec;
        LOG_WARN("coverage plugin: retrying " << failed_files.size() << " failed files as "
                                              << spec.task_id());
    }

    LOG_INFO("coverage plugin: result " << result.task_id() << " covered_lines="
                                        << covered_lines);

    if (wave_task_ids_.empty()) {
        const int settled_wave = SettleWave();
        SaveCheckpointLocked();
        if (settled_wave >= 0 && !keep_staging_) {
            if (!store_->clearWave(run_id_, settled_wave)) {
                LOG_ERROR("coverage plugin: clearWave failed for wave " << settled_wave);
            }
        }
    } else {
        SaveCheckpointLocked();
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
    task_seq_ = 0;
    total_files_ = 0;
    selected_count_ = 0;
    manifest_fingerprint_.clear();
    LOG_INFO("coverage plugin: run cancelled, task pool cleared");
}

int SchedulerPlugin::SettleWave() {
    const int settled_wave = static_cast<int>(current_wave_);
    std::unordered_map<std::string, std::set<std::uint64_t>> file_points;
    if (!store_->loadWave(run_id_, settled_wave, &file_points)) {
        LOG_ERROR("coverage plugin: loadWave failed for wave " << settled_wave);
        return -1;
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
        LOG_ERROR("coverage plugin: appendSelected failed for wave " << settled_wave);
        return -1;
    }
    std::vector<std::uint64_t> cumulative_vec(cumulative_.begin(), cumulative_.end());
    if (!store_->saveCumulative(run_id_, settled_wave, cumulative_vec)) {
        LOG_ERROR("coverage plugin: saveCumulative failed for wave " << settled_wave);
        return -1;
    }
    const size_t new_selected_count = selected_count_ + selected.size();
    if (!store_->saveRunSummary(run_id_, static_cast<int>(total_files_),
                                static_cast<int>(new_selected_count),
                                static_cast<std::int64_t>(cumulative_.size()))) {
        LOG_ERROR("coverage plugin: saveRunSummary failed for wave " << settled_wave);
        return -1;
    }
    if (store_cov_ && !MergeWaveCoverage(settled_wave)) {
        LOG_ERROR("coverage plugin: wave " << settled_wave
                                           << " cov merge failed, run suspended");
        return -1;
    }
    selected_count_ = new_selected_count;
    LOG_INFO("coverage plugin: wave " << settled_wave << " settled, selected "
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
            tasks_by_id_[task.task_id()] = task;
        }
        LOG_INFO("coverage plugin: wave " << current_wave_ << " started, " << pending_.size()
                                          << " tasks");
    } else {
        LOG_INFO("coverage plugin: run finished, selected " << selected_count_ << "/"
                                                            << total_files_
                                                            << " files, cumulative points="
                                                            << cumulative_.size());
    }
    return settled_wave;
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

    std::vector<std::string> wave_covs;
    if (!store_->exportWaveCovs(run_id_, wave, work_dir.string(), wave_covs)) {
        LOG_ERROR("coverage plugin: exportWaveCovs failed for wave " << wave);
        return false;
    }
    std::string cumulative_path;
    if (!store_->exportCumulativeCov(run_id_, work_dir.string(), cumulative_path)) {
        LOG_ERROR("coverage plugin: exportCumulativeCov failed for wave " << wave);
        return false;
    }
    if (wave_covs.empty() && cumulative_path.empty()) {
        LOG_ERROR("coverage plugin: wave " << wave << " has no cov artifacts to merge");
        return false;
    }

    std::string command = "\"" + opencppcoverage_ + "\" --quiet";
    if (!cumulative_path.empty()) {
        command += " --input_coverage=\"" + cumulative_path + "\"";
    }
    for (const std::string& path : wave_covs) {
        command += " --input_coverage=\"" + path + "\"";
    }
    const std::string out_cov = (work_dir / "cumulative.cov").string();
    const std::string out_xml = (work_dir / "summary.xml").string();
    command += " --export_type=binary:" + out_cov;
    command += " --export_type=cobertura:" + out_xml;

    const int exit_code = RunCommand(command);
    if (exit_code != 0) {
        LOG_ERROR("coverage plugin: cov merge failed for wave " << wave
                                                               << " exit=" << exit_code);
        return false;
    }
    std::int64_t covered = 0;
    std::int64_t total = 0;
    if (!ParseCoverageTotals(out_xml, &covered, &total)) {
        LOG_ERROR("coverage plugin: cannot parse merged cobertura for wave " << wave);
        return false;
    }
    if (!store_->saveCumulativeCov(run_id_, wave, out_cov)) {
        LOG_ERROR("coverage plugin: saveCumulativeCov failed for wave " << wave);
        return false;
    }
    const double percent = total > 0 ? static_cast<double>(covered) / total : 0.0;
    if (!store_->saveWaveSummary(run_id_, wave, covered, total, percent)) {
        LOG_ERROR("coverage plugin: saveWaveSummary failed for wave " << wave);
        return false;
    }
    std::filesystem::remove_all(work_dir, ec);
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
