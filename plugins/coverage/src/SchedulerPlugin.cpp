#include "coverage/SchedulerPlugin.h"
#include "common/Manifest.h"
#include "common/Logger.h"
#include "coverage.pb.h"
#include <algorithm>
#include <unordered_set>
#include <json.hpp>
namespace coverage_plugin {
std::string SchedulerPlugin::pluginType() const { return "coverage"; }
bool SchedulerPlugin::init(const std::string& config) {
    const auto cfg=nlohmann::json::parse(config);
    manifest_path_=cfg.value("stp_manifest",std::string());
    expected_program_version_=cfg.value("expected_program_version",std::string());
    expected_dataset_version_=cfg.value("expected_dataset_version",std::string());
    task_size_=cfg.value("task_size",25);
    return !manifest_path_.empty() && task_size_>0 && ledger_.init(config,pluginType());
}
bool SchedulerPlugin::validateClient(const common::ClientInfo&,const std::string& bytes,common::ValidationResult& result) {
    coverage::CoverageClientInfo info;
    result.accepted=info.ParseFromString(bytes) &&
        (expected_program_version_.empty() || expected_program_version_==info.program_version()) &&
        (expected_dataset_version_.empty() || expected_dataset_version_==info.dataset_version());
    result.reason=result.accepted?"ok":"invalid client information or version mismatch";
    return true;
}
bool SchedulerPlugin::plan(const common::RunRequest& request,const std::vector<common::ClientInfo>&) {
    std::vector<common::ManifestEntry> manifest; std::string fingerprint;
    if (!common::ReadManifest(manifest_path_,&manifest,&fingerprint)) {
        LOG_ERROR("coverage: invalid, duplicate or unreadable manifest " << manifest_path_); return false;
    }
    std::vector<std::string> files; std::vector<common::Task> tasks;
    for (const auto& entry:manifest) files.push_back(entry.path);
    for (std::size_t start=0;start<files.size();start+=task_size_) {
        coverage::CoverageTask payload; payload.set_run_id(request.run_id);
        for (std::size_t i=start;i<std::min(files.size(),start+task_size_);++i) payload.add_files(files[i]);
        common::Task task; task.set_run_id(request.run_id);
        task.set_task_id(request.run_id+"-t"+std::to_string(tasks.size()));
        payload.SerializeToString(task.mutable_payload()); tasks.push_back(task);
    }
    return ledger_.plan(request.run_id,tasks,files,fingerprint);
}
bool SchedulerPlugin::requestTask(const common::ClientInfo&,const std::string&,common::Task& task) { return ledger_.request(&task); }
bool SchedulerPlugin::processResult(const common::TaskResult& result) {
    if (result.exit_code()<0) return false;
    const auto* task=ledger_.task(result.task_id());
    coverage::CoverageTask expected; coverage::CoverageResult actual;
    if (!task || !expected.ParseFromString(task->payload()) || !actual.ParseFromString(result.result()) ||
        expected.files_size()!=actual.files_size()) return false;
    std::unordered_set<std::string> files(expected.files().begin(),expected.files().end());
    for (const auto& outcome:actual.files()) if (files.erase(outcome.file())!=1) return false;
    return files.empty() && ledger_.commit(result);
}
bool SchedulerPlugin::runFinished() { return ledger_.finished(); }
void SchedulerPlugin::cancel() { ledger_.clear(); }
void SchedulerPlugin::requeue(const std::vector<std::string>& ids) { ledger_.requeue(ids); }
bool SchedulerPlugin::saveCheckpoint(const std::string& run) { return ledger_.save(run); }
bool SchedulerPlugin::restore(const std::string& run) {
    if (ledger_.restore(run)) return true;
    // Legacy snapshots contain their exact remaining tasks. Configuration
    // changes must not reconstruct tasks and invent completed progress.
    std::string bytes; coverage::CoverageCheckpoint legacy;
    if (!ledger_.loadLegacy(run,&bytes) || !legacy.ParseFromString(bytes) || legacy.checkpoint_version()!=3) return false;
    std::vector<common::Task> tasks; std::vector<std::string> files;
    for (const auto& serialized:legacy.remaining_tasks()) {
        common::Task task; coverage::CoverageTask payload;
        if (!task.ParseFromString(serialized) || !payload.ParseFromString(task.payload())) return false;
        task.set_run_id(run); tasks.push_back(task);
    }
    // The original verified manifest supplies long-lived file metadata only.
    std::vector<common::ManifestEntry> manifest; std::string fingerprint;
    if (!common::ReadManifest(manifest_path_,&manifest,&fingerprint) ||
        (!legacy.manifest_fingerprint().empty() && legacy.manifest_fingerprint()!=fingerprint) ||
        static_cast<int>(manifest.size())!=legacy.total_files()) return false;
    for(const auto& entry:manifest) files.push_back(entry.path);
    return ledger_.plan(run,tasks,files,fingerprint);
}
}
