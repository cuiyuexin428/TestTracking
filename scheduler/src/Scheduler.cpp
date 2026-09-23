#include "scheduler/Scheduler.h"
#include "scheduler/ManagementServer.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "common/Logger.h"
#include "common/AtomicFile.h"
#include "common/Util.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace scheduler {
namespace {
std::string GenerateRunId() {
    return "run-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
}

void CancelAfterRecoveryFailure(common::TaskPlugin* plugin, const std::string& run_id) {
    try {
        plugin->cancel();
    } catch (const std::exception& error) {
        LOG_ERROR("recovery: cancel threw for run " << run_id << ": " << error.what());
    } catch (...) {
        LOG_ERROR("recovery: cancel threw unknown exception for run " << run_id);
    }
}
}  // namespace

Scheduler::Scheduler(common::Config& config, std::string config_path)
    : config_(config),
      config_path_(std::move(config_path)),
      instance_id_(GenerateRunId()),
      server_(config.get("server.router_endpoint", std::string("tcp://*:5555")),
              std::chrono::milliseconds(config.get("server.poll_timeout_ms", 50))) {
    server_.onHeartbeat([this](const IncomingMessage& msg) { HandleHeartbeat(msg); });
    effective_configuration_ = nlohmann::json::object();
    if (!config_path_.empty()) {
        std::string bytes;
        std::filesystem::path resolved;
        bool readable = false;
        try {
            resolved = std::filesystem::u8path(config_path_);
            readable = common::ReadCompleteFile(resolved, &bytes, 16 * 1024 * 1024);
        } catch (const std::system_error&) {
            // Older embedders still pass Windows ANSI paths, as Config::load
            // historically allowed. Normalize once at this boundary.
        }
#ifdef _WIN32
        if (!readable) {
            const int length = MultiByteToWideChar(CP_ACP, 0, config_path_.data(),
                                                   static_cast<int>(config_path_.size()), nullptr, 0);
            if (length > 0) {
                std::wstring wide(static_cast<size_t>(length), L'\0');
                MultiByteToWideChar(CP_ACP, 0, config_path_.data(),
                                    static_cast<int>(config_path_.size()), wide.data(), length);
                resolved = std::filesystem::path(wide);
                readable = common::ReadCompleteFile(resolved, &bytes, 16 * 1024 * 1024);
            }
        }
#endif
        if (!readable)
            throw std::runtime_error("Scheduler: cannot snapshot its configuration file");
        config_path_ = std::filesystem::absolute(resolved).u8string();
        startup_configuration_revision_ = common::Sha256Hex(bytes);
        effective_configuration_ = nlohmann::json::parse(bytes);
    }
    for (const auto* section : {"server", "plugins"}) {
        if (const auto* value = config_.find(section)) effective_configuration_[section] = *value;
        else effective_configuration_.erase(section);
    }
}
Scheduler::~Scheduler() {
    stop();
}

void Scheduler::start() {
    LoadPluginMaintenance();
    LoadPluginRollouts();
    LoadClientControl();
    if (!LoadPlugins()) throw std::runtime_error("Scheduler: plugin loading failed");
    const auto connection = config_.get("server.db_conn", std::string());
    if (!connection.empty()) {
        if (!storage_.init(connection)) throw std::runtime_error("Scheduler: storage init failed");
        RestoreRuns();
    } else RestoreRolloutRunsWithoutStorage();
    server_.start();
    running_.store(true);
    accepting_.store(true);
    PublishStatus();
    processing_ = std::thread(&Scheduler::RunProcessing, this);
    try {
        management_ = std::make_unique<ManagementServer>(*this);
        management_->start(config_.get("server.management_endpoint", std::string()));
    } catch (...) {
        stop();
        throw;
    }
}

void Scheduler::RestoreRuns() {
    std::vector<Storage::RunRecord> runs;
    std::vector<Storage::RunRecord> queued;
    if (!storage_.LoadUnfinishedRuns(runs, queued)) {
        throw std::runtime_error("Scheduler: failed to load unfinished runs");
    }
    const auto abort_recovery = [this](const std::string& message) -> void {
        for (const auto& active : active_runs_) {
            CancelAfterRecoveryFailure(active.first, active.second.run_id);
        }
        active_runs_.clear();
        active_run_count_.store(0);
        throw std::runtime_error(message);
    };
    // A run whose plugin state cannot be read must never block an older
    // resumable run. Closing it is part of recovery and therefore must persist
    // before startup can continue.
    const auto mark_interrupted = [this, &abort_recovery](const std::string& run_id) {
        if (!storage_.FinishRun(run_id, "interrupted")) {
            abort_recovery("Scheduler: cannot mark unrecoverable run " + run_id +
                           " interrupted");
        }
    };
    std::unordered_set<std::string> restored;
    for (const auto& run : runs) {
        if (PluginUnderMaintenance(run.plugin_type)) continue;
        const auto plugin = plugins_by_type_.find(run.plugin_type);
        if (plugin == plugins_by_type_.end()) {
            // The plugin may simply be disabled in this configuration, so keep
            // the recovery index rather than discarding a resumable run.
            LOG_WARN("recovery: no resumable plugin for run " << run.run_id
                                                              << " plugin_type=" << run.plugin_type
                                                              << "; retained for a later start");
            continue;
        }
        if (restored.count(run.plugin_type) != 0) continue;
        const RunDefinition definition{
            run.run_id, run.plugin_type, run.dataset_version, run.params, run.work_fingerprint,
            run.force_rerun, run.force_rerun_reason, run.rerun_of, run.worker_ids,
            run.execution_fingerprint};
        run_definitions_[run.run_id] = definition;
        if (run.status == "stopped" && RolloutBoundaryContains(run.plugin_type, run.run_id) &&
            !RolloutAutoResume(run.plugin_type, run.run_id)) {
            // The DB stop may have committed immediately before sidecar
            // publication failed. Reconcile before any old queued run can
            // activate and supersede this explicitly stopped batch.
            UpdateRolloutRun(run.run_id, "stopped");
            run_states_[run.run_id] = {run.plugin_type, run.work_fingerprint, "stopped"};
            run_order_.push_back(run.run_id);
            continue;
        }
        if (!ConfigurationMatches(definition)) {
            abort_recovery("Scheduler: recovery blocked; original progress retained: "
                           "plugin configuration or manifest differs from saved run " + run.run_id +
                           "; restore its original deployment before restarting");
        }
        bool plugin_restored = false;
        try {
            plugin_restored = plugin->second->restore(run.run_id);
        } catch (const common::RecoveryBlocked& e) {
            CancelAfterRecoveryFailure(plugin->second, run.run_id);
            abort_recovery("Scheduler: recovery blocked; original progress retained: " + std::string(e.what()));
        } catch (const std::exception& e) {
            LOG_ERROR("recovery: restore threw for run " << run.run_id << ": " << e.what());
        } catch (...) {
            LOG_ERROR("recovery: restore threw unknown exception for run " << run.run_id);
        }
        if (!plugin_restored) {
            LOG_ERROR("recovery: cannot restore run " << run.run_id << "; closing as interrupted");
            CancelAfterRecoveryFailure(plugin->second, run.run_id);
            mark_interrupted(run.run_id);
            continue;
        }
        // processResult() durably writes the plugin completion before the framework task row.
        // Reconciliation, reclaiming all remaining attempts, and resuming the run commit as one
        // state transition so a failed recovery remains retryable on the next start.
        if (!storage_.RestoreRun(run.run_id, run.plugin_type)) {
            CancelAfterRecoveryFailure(plugin->second, run.run_id);
            abort_recovery("Scheduler: cannot persist recovered state for run " + run.run_id);
        }
        active_runs_[plugin->second] = {run.run_id, run.plugin_type, false, run.worker_ids};
        run_states_[run.run_id] = {run.plugin_type, run.work_fingerprint, "running"};
        UpdateRolloutRun(run.run_id, "running");
        run_order_.push_back(run.run_id);
        restored.insert(run.plugin_type);
        LOG_INFO("run restored: " << run.run_id << " plugin_type=" << run.plugin_type);
    }
    for (const auto& run : queued) {
        const auto plugin = plugins_by_type_.find(run.plugin_type);
        if (plugin == plugins_by_type_.end()) {
            LOG_WARN("recovery: queued run retained for disabled plugin " << run.run_id
                                                                           << " plugin_type="
                                                                           << run.plugin_type);
            continue;
        }
        queued_runs_[plugin->second].push_back(
            {run.run_id, run.plugin_type, run.dataset_version, run.params,
             run.work_fingerprint, run.force_rerun, run.force_rerun_reason, run.rerun_of,
             run.worker_ids, run.execution_fingerprint});
        run_definitions_[run.run_id] = queued_runs_[plugin->second].back();
        run_states_[run.run_id] = {run.plugin_type, run.work_fingerprint, "queued"};
        run_order_.push_back(run.run_id);
    }
    active_run_count_.store(active_runs_.size());
    for (const auto& plugin : plugins_by_type_)
        if (!active_runs_.count(plugin.second)) StartNextRun(plugin.second);
}

bool Scheduler::Submit(ControlCommand command) {
    return SubmitDetailed(std::move(command)).value("ok", false);
}

nlohmann::json Scheduler::SubmitDetailed(ControlCommand command, bool bounded) {
    std::future<nlohmann::json> result;
    {
        std::lock_guard<std::mutex> lock(submit_mutex_);
        if (!accepting_.load()) return {{"ok", false}, {"error", "scheduler is stopping"}, {"http_status", 503}};
        if (command.kind == ControlCommand::Kind::Shutdown) accepting_.store(false);
        if (bounded) command.deadline = Clock::now() + std::chrono::seconds(30);
        command.completion = std::make_shared<std::promise<nlohmann::json>>();
        result = command.completion->get_future();
        commands_.push(std::move(command));
    }
    if (bounded && result.wait_for(std::chrono::seconds(30)) != std::future_status::ready)
        return {{"ok", false}, {"error", "scheduler command timed out; inspect current status before retrying"},
                {"http_status", 504}};
    return result.get();
}

void Scheduler::stop() {
    if (management_) management_->stop();
    if (accepting_.load()) Submit({ControlCommand::Kind::Shutdown, {}, {}, {}, {}});
    if (processing_.joinable()) processing_.join();
    server_.stop();
}
bool Scheduler::createRun(const std::string& plugin, const std::string& dataset,
                          const std::string& params) {
    return Submit({ControlCommand::Kind::Run, plugin, dataset, params, {}});
}
bool Scheduler::stopRun(const std::string& plugin) {
    return Submit({ControlCommand::Kind::Stop, plugin, {}, {}, {}});
}
bool Scheduler::wakeupClients(const std::string& plugin) {
    return Submit({ControlCommand::Kind::Wake, plugin, {}, {}, {}});
}

std::vector<common::ClientInfo> Scheduler::clients() const {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    std::vector<common::ClientInfo> result;
    for (const auto& pair : clients_) {
        auto info = pair.second.info;
        info.last_seen_age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    Clock::now() - pair.second.last_seen)
                                    .count();
        result.push_back(std::move(info));
    }
    return result;
}
size_t Scheduler::pluginCount() const {
    return plugins_by_type_.size();
}
size_t Scheduler::inFlightCount() const {
    return in_flight_count_.load();
}
std::vector<std::string> Scheduler::pluginTypes() const {
    std::vector<std::string> types;
    for (const auto& pair : plugins_by_type_) types.push_back(pair.first);
    return types;
}

void Scheduler::RunProcessing() {
    auto last_check = Clock::now();
    while (running_.load()) {
        ControlCommand command;
        if (commands_.pop(command, std::chrono::milliseconds(0))) {
            nlohmann::json outcome = {{"ok", false}, {"error", "scheduler command failed"}, {"http_status", 503}};
            try {
                if (Clock::now() > command.deadline)
                    outcome = {{"ok", false}, {"error", "command expired before execution"}, {"http_status", 504}};
                else
                    outcome = HandleCommand(command);
            } catch (const std::exception& e) {
                LOG_ERROR("command failed: " << e.what());
                outcome["error"] = e.what();
            } catch (...) {
                LOG_ERROR("command failed: unknown exception");
            }
            if (command.kind == ControlCommand::Kind::Shutdown) running_.store(false);
            PublishStatus();
            command.completion->set_value(std::move(outcome));
            continue;
        }
        try {
            IncomingMessage msg;
            if (server_.tryRecv(msg, std::chrono::milliseconds(20))) HandleMessage(msg);
            if (Clock::now() - last_check >= std::chrono::milliseconds(200)) {
                last_check = Clock::now();
                RunTimeoutChecks();
                PublishStatus();
            }
        } catch (const std::exception& e) {
            LOG_ERROR("scheduler operation failed; retained state for retry: " << e.what());
        } catch (...) {
            LOG_ERROR("scheduler operation failed");
        }
    }
}

nlohmann::json Scheduler::HandleCommand(const ControlCommand& command) {
    if (command.kind == ControlCommand::Kind::ClientControl)
        return HandleClientControl(command.client_control);
    if (command.kind == ControlCommand::Kind::ApplyPluginDeployment)
        return ApplyPluginDeployment(command.client_control);
    if (command.kind == ControlCommand::Kind::PluginMaintenance)
        return HandlePluginMaintenance(command.client_control);
    if (command.kind == ControlCommand::Kind::PluginRollout)
        return HandlePluginRollout(command.client_control);
    const auto reject = [](const std::string& error) -> nlohmann::json {
        return {{"ok", false}, {"error", error}, {"http_status", 409}};
    };
    if (command.kind != ControlCommand::Kind::Shutdown &&
        !(command.kind == ControlCommand::Kind::Wake && command.plugin_type.empty()) &&
        !plugins_by_type_.count(command.plugin_type))
        return reject("plugin is not enabled: " + command.plugin_type);
    switch (command.kind) {
        case ControlCommand::Kind::Run: {
            auto* plugin = plugins_by_type_.at(command.plugin_type);
            if (active_runs_.count(plugin)) CommitFinished(plugin);
            return HandleCreateRun(command);
        }
        case ControlCommand::Kind::Stop: {
            auto* plugin = plugins_by_type_.at(command.plugin_type);
            const auto run = active_runs_.find(plugin);
            if (!command.run_id.empty() &&
                (run == active_runs_.end() || run->second.run_id != command.run_id)) {
                if (!CancelQueuedRun(plugin, command.run_id))
                    return reject("run is no longer active or queued; refresh before stopping");
                return {{"ok", true}, {"run_id", command.run_id}, {"status", "cancelled"}};
            }
            const std::string run_id = run == active_runs_.end() ? std::string() : run->second.run_id;
            if (!HandleStopRun(command.plugin_type))
                return reject("stop requested but persistence is still pending; inspect current status and scheduler logs");
            return {{"ok", true}, {"run_id", run_id}, {"status", "stopped"}};
        }
        case ControlCommand::Kind::Wake:
            SendWake(command.plugin_type);
            return {{"ok", true}};
        case ControlCommand::Kind::Shutdown: {
            shutting_down_ = true;
            PrepareRolloutShutdown();
            bool ok = true;
            for (const auto& type : pluginTypes()) {
                if (active_runs_.count(plugins_by_type_.at(type))) ok = HandleStopRun(type) && ok;
            }
            if (!ok) LOG_WARN("shutdown: incomplete persistence retained for recovery");
            return {{"ok", ok}};
        }
        case ControlCommand::Kind::ClientControl:
        case ControlCommand::Kind::ApplyPluginDeployment:
        case ControlCommand::Kind::PluginMaintenance:
        case ControlCommand::Kind::PluginRollout:
            break;
    }
    return reject("unknown command");
}

nlohmann::json Scheduler::HandleCreateRun(const ControlCommand& request) {
    auto command = request;
    command.params = common::NormalizeRunParameters(command.params);
    const auto reject = [](const std::string& error) -> nlohmann::json {
        return {{"ok", false}, {"error", error}, {"http_status", 409}};
    };
    if (PluginUnderMaintenance(command.plugin_type))
        return reject("plugin is reserved for maintenance; finish installation and restart scheduler: " + command.plugin_type);
    if (RolloutPending(command.plugin_type))
        return reject("plugin update is pending; existing batches continue and new batches wait for the new target");
    const auto found = plugins_by_type_.find(command.plugin_type);
    if (found == plugins_by_type_.end()) {
        LOG_ERROR("createRun: no plugin for plugin_type " << command.plugin_type);
        return reject("plugin is not enabled: " + command.plugin_type);
    }
    auto* plugin = found->second;
    std::string parameter_error;
    if (!plugin->validateRunParameters(command.params, parameter_error))
        return {{"ok", false}, {"error", parameter_error.empty() ? "unsupported run parameters" : parameter_error},
                {"http_status", 400}};
    RunDefinition run{GenerateRunId(), command.plugin_type, command.dataset_version,
                      command.params, {}, command.force_rerun, command.force_rerun_reason, {},
                      command.worker_ids};
    run.work_fingerprint = WorkFingerprint(command);
    run.execution_fingerprint = ExecutionFingerprint(command.plugin_type, run.work_fingerprint);
    if (!duplicate_protection_enabled_.at(command.plugin_type))
        run.work_fingerprint = common::Sha256Hex(run.work_fingerprint + "\n" + run.run_id);

    if (storage_.enabled()) {
        Storage::RunRecord record{run.run_id, run.plugin_type, run.dataset_version, run.params,
                                  run.work_fingerprint, run.force_rerun,
                                  run.force_rerun_reason, {}, "queued", run.worker_ids,
                                  run.execution_fingerprint};
        const auto enqueued = storage_.EnqueueRun(record);
        if (enqueued.outcome == Storage::EnqueueResult::Outcome::Error) {
            LOG_ERROR("createRun: enqueue failed; no task dispatched");
            return reject("run could not be queued; check scheduler logs and database");
        }
        if (enqueued.outcome == Storage::EnqueueResult::Outcome::Duplicate) {
            return {{"ok", false},
                    {"error", "equivalent work already exists"},
                    {"duplicate", true},
                    {"run_id", enqueued.run_id},
                    {"status", enqueued.status},
                    {"force_rerun_available", true},
                    {"http_status", 409}};
        }
        if (enqueued.outcome == Storage::EnqueueResult::Outcome::ForceTargetMissing)
            return reject("force rerun requires an existing equivalent run");
        run.rerun_of = enqueued.rerun_of;
    } else {
        std::string duplicate_id, duplicate_status;
        for (auto it = run_order_.rbegin(); it != run_order_.rend(); ++it) {
            const auto state = run_states_.find(*it);
            if (state != run_states_.end() && state->second.work_fingerprint == run.work_fingerprint &&
                (state->second.status == "queued" || state->second.status == "planning" ||
                 state->second.status == "running" || state->second.status == "stopped" ||
                 (state->second.status == "finished" &&
                  (run.force_rerun || (state->second.has_task_reports &&
                                       state->second.all_tasks_succeeded))))) {
                duplicate_id = *it;
                duplicate_status = state->second.status;
                break;
            }
        }
        if (!duplicate_id.empty() && !run.force_rerun) {
            return {{"ok", false},
                    {"error", "equivalent work already exists"},
                    {"duplicate", true},
                    {"run_id", duplicate_id},
                    {"status", duplicate_status},
                    {"force_rerun_available", true},
                    {"http_status", 409}};
        }
        if (run.force_rerun && duplicate_id.empty())
            return reject("force rerun requires an existing equivalent run");
        if (run.force_rerun) run.rerun_of = duplicate_id;
    }

    queued_runs_[plugin].push_back(run);
    run_definitions_[run.run_id] = run;
    run_states_[run.run_id] = {run.plugin_type, run.work_fingerprint, "queued"};
    run_order_.push_back(run.run_id);
    if (!active_runs_.count(plugin)) StartNextRun(plugin);

    const auto state = run_states_.find(run.run_id);
    if (state == run_states_.end() || state->second.status == "failed")
        return reject("run could not be created; check scheduler logs and plugin configuration");
    return {{"ok", true},
            {"run_id", run.run_id},
            {"status", state->second.status},
            {"rerun_of", run.rerun_of},
            {"worker_ids", run.worker_ids}};
}

std::string Scheduler::WorkFingerprint(const ControlCommand& command) const {
    std::string manifest_sha256;
    const auto manifest = manifest_paths_.find(command.plugin_type);
    if (manifest != manifest_paths_.end() && !manifest->second.empty() &&
        !common::FileSha256Hex(manifest->second, &manifest_sha256))
        manifest_sha256 = "unreadable";
    nlohmann::json identity = {
        {"plugin_type", command.plugin_type},
        {"deployment_identity", deployment_identities_.at(command.plugin_type)},
        {"manifest_sha256", manifest_sha256},
        {"dataset_version", command.dataset_version},
        {"params_text", command.params}};
    const auto rollout_key = RolloutKey(command.plugin_type);
    if (plugin_rollouts_.contains(rollout_key) && plugin_rollouts_.at(rollout_key).at("admission_target") != "")
        identity["deployment_target_id"] = plugin_rollouts_.at(rollout_key).at("admission_target");
    return common::Sha256Hex(identity.dump());
}

std::string Scheduler::ExecutionFingerprint(const std::string& plugin_type,
                                             const std::string& work_fingerprint) const {
    const nlohmann::json identity = {
        {"work_identity", work_fingerprint},
        {"plugin_configuration", configuration_identities_.at(plugin_type)}};
    return common::Sha256Hex(identity.dump());
}

bool Scheduler::ConfigurationMatches(const RunDefinition& run) const {
    ControlCommand command{};
    command.plugin_type = run.plugin_type;
    command.dataset_version = run.dataset_version;
    command.params = run.params;
    if (!run.execution_fingerprint.empty())
        return run.execution_fingerprint == ExecutionFingerprint(run.plugin_type, WorkFingerprint(command));
    // Older records predate the complete configuration snapshot. Retain their
    // existing work identity check, including intentionally non-deduplicated runs.
    if (run.work_fingerprint.empty()) return true;
    const auto current = WorkFingerprint(command);
    return run.work_fingerprint == current ||
           run.work_fingerprint == common::Sha256Hex(current + "\n" + run.run_id);
}

void Scheduler::SetRunStatus(const std::string& run_id, const std::string& status) {
    UpdateRolloutRun(run_id, status);
    const auto found = run_states_.find(run_id);
    if (found != run_states_.end()) found->second.status = status;
}

void Scheduler::SetRunStatusBestEffort(const std::string& run_id, const std::string& status) {
    try {
        SetRunStatus(run_id, status);
    } catch (const std::exception& error) {
        // The database transition is already durable.  A rollout sidecar is
        // repaired by the next status reconciliation; it must not strand the
        // in-memory queue behind a run that has already reached a terminal
        // state.
        LOG_ERROR("run status sidecar publication failed for " << run_id << ": " << error.what());
    } catch (...) {
        LOG_ERROR("run status sidecar publication failed for " << run_id << ": unknown exception");
    }
    const auto found = run_states_.find(run_id);
    if (found != run_states_.end()) found->second.status = status;
}

bool Scheduler::StartNextRun(common::TaskPlugin* plugin) {
    if (shutting_down_ || active_runs_.count(plugin) || PluginUnderMaintenance(plugin->pluginType())) return true;
    if (RolloutSwitching(plugin->pluginType()) || RolloutStopped(plugin->pluginType())) return true;
    auto& queue = queued_runs_[plugin];
    while (!queue.empty() && !shutting_down_) {
        auto& next = queue.front();
        if (Clock::now() < next.next_configuration_check) return false;
        if (!ConfigurationMatches(next)) {
            const std::string reason = "plugin configuration or manifest changed since this run was queued; "
                                       "restore the original deployment or cancel this queued run";
            if (next.blocked_reason != reason)
                LOG_WARN("queued run blocked: " << next.run_id << ": " << reason);
            next.blocked_reason = reason;
            next.next_configuration_check = Clock::now() + std::chrono::seconds(2);
            return false;
        }
        next.blocked_reason.clear();
        const RunDefinition run = queue.front();
        if (storage_.enabled() && !storage_.BeginPlanning(run.run_id, run.plugin_type)) {
            LOG_ERROR("createRun: cannot begin planning queued run " << run.run_id);
            return false;
        }
        SetRunStatusBestEffort(run.run_id, "planning");

        bool planned = false;
        try {
            auto selected_clients = clients();
            selected_clients.erase(std::remove_if(selected_clients.begin(), selected_clients.end(),
                [this, &run](const common::ClientInfo& client) {
                    return client.plugin_type != run.plugin_type ||
                           !RolloutAllowsWorker(run.plugin_type, client.identity) ||
                           !IsSelectedWorker(client.identity, run.worker_ids);
                }), selected_clients.end());
            planned = plugin->plan({run.run_id, run.dataset_version, run.params}, selected_clients);
        } catch (const std::exception& error) {
            LOG_ERROR("createRun: plan threw for " << run.plugin_type << ": " << error.what());
        } catch (...) {
            LOG_ERROR("createRun: plan threw for " << run.plugin_type << ": unknown exception");
        }
        if (!planned) {
            if (storage_.enabled() && !storage_.FinishRun(run.run_id, "failed"))
                LOG_ERROR("createRun: cannot mark failed run " << run.run_id
                                                                << "; startup recovery will close it");
            SetRunStatusBestEffort(run.run_id, "failed");
            try {
                plugin->cancel();
            } catch (const std::exception& error) {
                LOG_ERROR("createRun: cancel after plan failure threw for " << run.plugin_type
                                                                            << ": " << error.what());
            } catch (...) {
                LOG_ERROR("createRun: cancel after plan failure threw for " << run.plugin_type
                                                                            << ": unknown exception");
            }
            queue.pop_front();
            LOG_ERROR("createRun: plan failed for " << run.plugin_type);
            continue;
        }

        if (storage_.enabled() && !storage_.ActivateRun(run.run_id, run.plugin_type)) {
            LOG_ERROR("createRun: cannot activate run " << run.run_id << "; no task dispatched");
            if (!storage_.FinishRun(run.run_id, "failed"))
                LOG_ERROR("createRun: cannot mark unactivated run " << run.run_id << " failed");
            SetRunStatusBestEffort(run.run_id, "failed");
            try {
                plugin->cancel();
            } catch (const std::exception& error) {
                LOG_ERROR("createRun: cancel after activation failure threw for " << run.plugin_type
                                                                                  << ": " << error.what());
            } catch (...) {
                LOG_ERROR("createRun: cancel after activation failure threw for " << run.plugin_type
                                                                                  << ": unknown exception");
            }
            queue.pop_front();
            continue;
        }

        // The successful database activation also superseded old stopped runs.
        // Keep the runtime status/maintenance guards consistent with that commit.
        for (auto& state : run_states_)
            if (state.second.plugin_type == run.plugin_type && state.second.status == "stopped")
                state.second.status = "cancelled";
        queue.pop_front();
        active_runs_[plugin] = {run.run_id, run.plugin_type, false, run.worker_ids};
        SetRunStatusBestEffort(run.run_id, "running");
        active_run_count_.store(active_runs_.size());
        LOG_INFO("run started: " << run.run_id << " plugin_type=" << run.plugin_type);
        SendWake(run.plugin_type);
        return true;
    }
    return true;
}

bool Scheduler::CancelQueuedRun(common::TaskPlugin* plugin, const std::string& run_id) {
    auto found_queue = queued_runs_.find(plugin);
    if (found_queue == queued_runs_.end()) return false;
    auto& queue = found_queue->second;
    const auto found = std::find_if(queue.begin(), queue.end(), [&](const RunDefinition& run) {
        return run.run_id == run_id;
    });
    if (found == queue.end()) return false;
    if (storage_.enabled() && !storage_.CancelQueuedRun(run_id, found->plugin_type)) return false;
    SetRunStatusBestEffort(run_id, "cancelled");
    queue.erase(found);
    if (!active_runs_.count(plugin)) StartNextRun(plugin);
    return true;
}

void Scheduler::SendWake(const std::string& plugin_type) {
    std::vector<std::pair<std::string, std::string>> peers;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (const auto& pair : clients_) {
            if (plugin_type.empty() || pair.second.info.plugin_type == plugin_type)
                peers.emplace_back(pair.first, pair.second.session_id);
        }
    }
    for (const auto& peer : peers) {
        common::Envelope env;
        env.set_type(common::WAKE);
        env.set_plugin_type(plugin_type);
        env.set_session_id(peer.second);
        env.mutable_wake();
        server_.sendTo(peer.first, env);
        LOG_INFO("wakeup sent to " << peer.first);
    }
}

bool Scheduler::HandleStopRun(const std::string& type) {
    const auto found = plugins_by_type_.find(type);
    if (found == plugins_by_type_.end()) return false;
    auto run = active_runs_.find(found->second);
    if (run == active_runs_.end()) return true;
    run->second.stopping = true;
    std::vector<std::pair<std::string, std::string>> peers;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (const auto& pair : clients_) {
            if (pair.second.info.plugin_type == type)
                peers.emplace_back(pair.first, pair.second.session_id);
        }
    }
    for (const auto& peer : peers) {
        common::Envelope env;
        env.set_type(common::STOP);
        env.set_plugin_type(type);
        env.set_session_id(peer.second);
        env.mutable_stop()->set_run_id(run->second.run_id);
        env.mutable_stop()->set_reason("manual stop");
        server_.sendTo(peer.first, env);
        ReclaimClientTasks(peer.first, peer.second);
    }
    return CommitStop(found->second);
}

bool Scheduler::CommitStop(common::TaskPlugin* plugin) {
    const auto run = active_runs_.find(plugin);
    if (run == active_runs_.end()) return true;
    if (HasInFlight(run->second.run_id) || !plugin->saveCheckpoint(run->second.run_id))
        return false;
    if (storage_.enabled() && !storage_.FinishRun(run->second.run_id, "stopped")) return false;
    // Publishing the rollout boundary can fail independently of the database.
    // Retain the plugin checkpoint until that retryable publication succeeds.
    SetRunStatus(run->second.run_id, "stopped");
    plugin->cancel();
    LOG_INFO("run stopped: " << run->second.run_id << " plugin_type=" << run->second.plugin_type);
    active_runs_.erase(run);
    active_run_count_.store(active_runs_.size());
    StartNextRun(plugin);
    return true;
}

bool Scheduler::HasInFlight(const std::string& run) const {
    for (const auto& pair : in_flight_)
        if (pair.second.run_id == run) return true;
    return false;
}
bool Scheduler::CommitFinished(common::TaskPlugin* plugin) {
    const auto run = active_runs_.find(plugin);
    if (run == active_runs_.end()) return true;
    if (run->second.stopping || HasInFlight(run->second.run_id) || !plugin->runFinished())
        return false;
    if (storage_.enabled() && !storage_.FinishRun(run->second.run_id, "finished")) return false;
    LOG_INFO("run finished: " << run->second.run_id << " plugin_type=" << run->second.plugin_type);
    SetRunStatus(run->second.run_id, "finished");
    active_runs_.erase(run);
    active_run_count_.store(active_runs_.size());
    StartNextRun(plugin);
    return true;
}

void Scheduler::ReclaimTasks(const std::vector<std::string>& ids) {
    std::unordered_map<std::string, std::vector<std::string>> groups;
    for (const auto& id : ids) {
        const auto found = in_flight_.find(id);
        if (found == in_flight_.end() || found->second.received) continue;
        groups[found->second.plugin_type].push_back(id);
    }
    for (const auto& group : groups) {
        // Required reclaim persistence must precede re-dispatch.
        if (storage_.enabled() && !storage_.ReclaimTasks(group.second)) continue;
        plugins_by_type_.at(group.first)->requeue(group.second);
        for (const auto& id : group.second) in_flight_.erase(id);
    }
    in_flight_count_.store(in_flight_.size());
}
void Scheduler::ReclaimClientTasks(const std::string& identity, const std::string& session) {
    std::vector<std::string> ids;
    for (const auto& pair : in_flight_)
        if (pair.second.identity == identity && pair.second.session_id == session)
            ids.push_back(pair.first);
    ReclaimTasks(ids);
}

void Scheduler::RunTimeoutChecks() {
    try {
        ExpireClientReservations();
    } catch (const std::exception& error) {
        // Reservation persistence is retryable.  Do not skip task reclaim,
        // run completion, or queue advancement when its sidecar is briefly
        // unavailable; the next maintenance tick retries expiration.
        LOG_ERROR("cannot expire client reservations; retaining state for retry: " << error.what());
    } catch (...) {
        LOG_ERROR("cannot expire client reservations; retaining state for retry");
    }
    std::vector<std::string> received;
    for (const auto& pair : in_flight_)
        if (pair.second.received) received.push_back(pair.first);
    for (const auto& id : received) CommitReport(id);

    const int timeout = config_.get("server.client_timeout_ms", 0);
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (auto it = clients_.begin(); it != clients_.end();) {
            if (timeout > 0 &&
                Clock::now() - it->second.last_seen > std::chrono::milliseconds(timeout)) {
                LOG_WARN("client offline: " << it->first);
                it = clients_.erase(it);
            } else
                ++it;
        }
    }
    std::vector<std::string> expired;
    const int lease = config_.get("server.task_lease_timeout_ms", 0);
    const int receipt_timeout = config_.get("server.task_ack_timeout_ms", 10000);
    for (const auto& pair : in_flight_) {
        const auto& task = pair.second;
        bool offline;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            const auto peer = clients_.find(task.identity);
            offline = peer == clients_.end() || peer->second.session_id != task.session_id;
        }
        const auto age =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - task.sent_at)
                .count();
        const auto plugin = plugins_by_type_.at(task.plugin_type);
        const auto run = active_runs_.find(plugin);
        if (offline || (lease > 0 && age > lease) || (!task.acked && age > receipt_timeout) ||
            (run != active_runs_.end() && run->second.stopping))
            expired.push_back(pair.first);
    }
    ReclaimTasks(expired);
    std::vector<common::TaskPlugin*> active;
    for (const auto& pair : active_runs_) active.push_back(pair.first);
    for (auto* plugin : active) {
        if (active_runs_.at(plugin).stopping)
            CommitStop(plugin);
        else
            CommitFinished(plugin);
    }
    for (const auto& plugin : plugins_by_type_)
        if (!active_runs_.count(plugin.second) && !queued_runs_[plugin.second].empty())
            StartNextRun(plugin.second);
}

bool Scheduler::LoadPlugins() {
    const nlohmann::json* plugins_node = config_.find("plugins");
    if (plugins_node == nullptr || !plugins_node->is_object()) {
        LOG_WARN("no plugins configured");
        return true;
    }

    for (auto it = plugins_node->begin(); it != plugins_node->end(); ++it) {
        const std::string& name = it.key();
        const nlohmann::json& entry = it.value();
        if (!entry.value("enabled", false)) {
            LOG_INFO("plugin " << name << " disabled");
            continue;
        }

        const std::string dll_path = entry.value("dll", std::string());
        std::unique_ptr<common::DynamicLibrary> library = common::DynamicLibrary::load(dll_path);
        if (library == nullptr) {
            LOG_ERROR("plugin " << name << ": cannot load " << dll_path << ": "
                                << common::DynamicLibrary::lastError());
            return false;
        }

        auto create = library->get<common::CreatePluginFn>("CreatePlugin");
        auto destroy = library->get<common::DestroyPluginFn>("DestroyPlugin");
        if (create == nullptr || destroy == nullptr) {
            LOG_ERROR("plugin " << name << ": missing CreatePlugin/DestroyPlugin exports");
            return false;
        }

        common::TaskPlugin* plugin = nullptr;
        try {
            plugin = create();
        } catch (const std::exception& e) {
            LOG_ERROR("plugin " << name << ": CreatePlugin threw: " << e.what());
            return false;
        } catch (...) {
            LOG_ERROR("plugin " << name << ": CreatePlugin threw unknown exception");
            return false;
        }
        if (plugin == nullptr) {
            LOG_ERROR("plugin " << name << ": CreatePlugin returned null");
            return false;
        }

        const std::string plugin_config = entry.dump();
        try {
            if (!plugin->init(plugin_config)) {
                LOG_ERROR("plugin " << name << ": init failed");
                destroy(plugin);
                return false;
            }
        } catch (const std::exception& e) {
            LOG_ERROR("plugin " << name << ": init threw: " << e.what());
            destroy(plugin);
            return false;
        } catch (...) {
            LOG_ERROR("plugin " << name << ": init threw unknown exception");
            destroy(plugin);
            return false;
        }

        const std::string plugin_type = plugin->pluginType();
        if (plugins_by_type_.count(plugin_type) > 0) {
            LOG_ERROR("plugin " << name << ": duplicate plugin_type=" << plugin_type);
            destroy(plugin);
            return false;
        }

        std::string plugin_sha256;
        if (!common::FileSha256Hex(dll_path, &plugin_sha256)) {
            LOG_ERROR("plugin " << name << ": cannot hash loaded binary " << dll_path);
            destroy(plugin);
            return false;
        }
        const auto identity = DescribePlugin(plugin_type, entry, plugin_sha256);
        deployment_identities_[plugin_type] = identity.deployment;
        configuration_identities_[plugin_type] = identity.configuration;
        manifest_paths_[plugin_type] = identity.manifest;
        duplicate_protection_enabled_[plugin_type] = identity.duplicate_protection;
        if (!identity.duplicate_protection)
            LOG_WARN("plugin " << name << ": duplicate protection is disabled because expected "
                               << "program version fields are incomplete");
        plugins_by_type_[plugin_type] = plugin;

        plugins_.push_back({std::move(library), std::unique_ptr<common::TaskPlugin, PluginDeleter>(
                                                    plugin, PluginDeleter{destroy}), name, plugin_sha256});
        LOG_INFO("plugin loaded: " << name << " plugin_type=" << plugin->pluginType());
    }
    return true;
}

}  // namespace scheduler
