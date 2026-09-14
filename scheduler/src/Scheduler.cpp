#include "scheduler/Scheduler.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "common/Logger.h"

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

Scheduler::Scheduler(common::Config& config)
    : config_(config),
      server_(config.get("server.router_endpoint", std::string("tcp://*:5555")),
              std::chrono::milliseconds(config.get("server.poll_timeout_ms", 50))) {
    server_.onHeartbeat([this](const IncomingMessage& msg) { HandleHeartbeat(msg); });
}
Scheduler::~Scheduler() {
    stop();
}

void Scheduler::start() {
    if (!LoadPlugins()) throw std::runtime_error("Scheduler: plugin loading failed");
    const auto connection = config_.get("server.db_conn", std::string());
    if (!connection.empty()) {
        if (!storage_.init(connection)) throw std::runtime_error("Scheduler: storage init failed");
        RestoreRuns();
    }
    server_.start();
    running_.store(true);
    accepting_.store(true);
    processing_ = std::thread(&Scheduler::RunProcessing, this);
}

void Scheduler::RestoreRuns() {
    std::vector<Storage::RunRecord> runs;
    if (!storage_.LoadUnfinishedRuns(runs)) {
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
        bool plugin_restored = false;
        try {
            plugin_restored = plugin->second->restore(run.run_id);
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
        // A crash between those commits is repaired from the completion journal before the run
        // becomes active again.
        if (!storage_.ReconcileCompletedTasks(run.run_id, run.plugin_type)) {
            CancelAfterRecoveryFailure(plugin->second, run.run_id);
            abort_recovery("Scheduler: cannot reconcile completed tasks for run " + run.run_id);
        }
        if (!storage_.ResumeRun(run.run_id)) {
            CancelAfterRecoveryFailure(plugin->second, run.run_id);
            abort_recovery("Scheduler: cannot persist resume for run " + run.run_id);
        }
        active_runs_[plugin->second] = {run.run_id, run.plugin_type, false};
        restored.insert(run.plugin_type);
        LOG_INFO("run restored: " << run.run_id << " plugin_type=" << run.plugin_type);
    }
    active_run_count_.store(active_runs_.size());
}

bool Scheduler::Submit(ControlCommand command) {
    std::future<bool> result;
    {
        std::lock_guard<std::mutex> lock(submit_mutex_);
        if (!accepting_.load()) return false;
        if (command.kind == ControlCommand::Kind::Shutdown) accepting_.store(false);
        command.completion = std::make_shared<std::promise<bool>>();
        result = command.completion->get_future();
        commands_.push(std::move(command));
    }
    return result.get();
}

void Scheduler::stop() {
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
            bool ok = false;
            try {
                ok = HandleCommand(command);
            } catch (const std::exception& e) {
                LOG_ERROR("command failed: " << e.what());
            } catch (...) {
                LOG_ERROR("command failed: unknown exception");
            }
            if (command.kind == ControlCommand::Kind::Shutdown) running_.store(false);
            command.completion->set_value(ok);
            continue;
        }
        try {
            IncomingMessage msg;
            if (server_.tryRecv(msg, std::chrono::milliseconds(20))) HandleMessage(msg);
            if (Clock::now() - last_check >= std::chrono::milliseconds(200)) {
                last_check = Clock::now();
                RunTimeoutChecks();
            }
        } catch (const std::exception& e) {
            LOG_ERROR("scheduler operation failed; retained state for retry: " << e.what());
        } catch (...) {
            LOG_ERROR("scheduler operation failed");
        }
    }
}

bool Scheduler::HandleCommand(const ControlCommand& command) {
    switch (command.kind) {
        case ControlCommand::Kind::Run:
            return HandleCreateRun(command);
        case ControlCommand::Kind::Stop:
            return HandleStopRun(command.plugin_type);
        case ControlCommand::Kind::Wake:
            SendWake(command.plugin_type);
            return true;
        case ControlCommand::Kind::Shutdown: {
            bool ok = true;
            for (const auto& type : pluginTypes()) {
                if (active_runs_.count(plugins_by_type_.at(type))) ok = HandleStopRun(type) && ok;
            }
            if (!ok) LOG_WARN("shutdown: incomplete persistence retained for recovery");
            return ok;
        }
    }
    return false;
}

bool Scheduler::HandleCreateRun(const ControlCommand& command) {
    const auto found = plugins_by_type_.find(command.plugin_type);
    if (found == plugins_by_type_.end()) {
        LOG_ERROR("createRun: no plugin for plugin_type " << command.plugin_type);
        return false;
    }
    auto* plugin = found->second;
    if (active_runs_.count(plugin) && !CommitFinished(plugin)) {
        LOG_ERROR("createRun: run already active for plugin_type " << command.plugin_type);
        return false;
    }
    common::RunRequest request{GenerateRunId(), command.dataset_version, command.params};
    // Persist the recovery index before creating plugin state or dispatching.
    if (storage_.enabled() && !storage_.SaveRun(request.run_id, command.plugin_type,
                                                request.dataset_version, request.params)) {
        LOG_ERROR("createRun: SaveRun failed; no task dispatched");
        return false;
    }
    bool planned = false;
    try {
        planned = plugin->plan(request, clients());
    } catch (const std::exception& e) {
        LOG_ERROR("createRun: plan threw for " << command.plugin_type << ": " << e.what());
    } catch (...) {
        LOG_ERROR("createRun: plan threw for " << command.plugin_type << ": unknown exception");
    }
    if (!planned) {
        // Leave a terminal status so recovery never retries a run whose plan was
        // never persisted; the recovery path closes any row this write misses.
        if (storage_.enabled() && !storage_.FinishRun(request.run_id, "failed")) {
            LOG_ERROR("createRun: cannot mark run " << request.run_id
                                                    << " failed; recovery will close it as "
                                                       "interrupted on the next start");
        }
        try {
            plugin->cancel();
        } catch (const std::exception& e) {
            LOG_ERROR("createRun: cancel after plan failure threw for " << command.plugin_type
                                                                        << ": " << e.what());
        } catch (...) {
            LOG_ERROR("createRun: cancel after plan failure threw for " << command.plugin_type
                                                                        << ": unknown exception");
        }
        LOG_ERROR("createRun: plan failed for " << command.plugin_type);
        return false;
    }
    if (storage_.enabled() && !storage_.ActivateRun(request.run_id, command.plugin_type)) {
        LOG_ERROR("createRun: cannot activate run " << request.run_id << "; no task dispatched");
        if (!storage_.FinishRun(request.run_id, "failed")) {
            LOG_ERROR("createRun: cannot mark unactivated run " << request.run_id
                                                                << " failed; recovery will close "
                                                                   "a planning row on the next "
                                                                   "start");
        }
        try {
            plugin->cancel();
        } catch (const std::exception& e) {
            LOG_ERROR("createRun: cancel after activation failure threw for "
                      << command.plugin_type << ": " << e.what());
        } catch (...) {
            LOG_ERROR("createRun: cancel after activation failure threw for "
                      << command.plugin_type << ": unknown exception");
        }
        return false;
    }
    active_runs_[plugin] = {request.run_id, command.plugin_type, false};
    active_run_count_.store(active_runs_.size());
    LOG_INFO("run started: " << request.run_id << " plugin_type=" << command.plugin_type);
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
    plugin->cancel();
    LOG_INFO("run stopped: " << run->second.run_id << " plugin_type=" << run->second.plugin_type);
    active_runs_.erase(run);
    active_run_count_.store(active_runs_.size());
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
    active_runs_.erase(run);
    active_run_count_.store(active_runs_.size());
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
        plugins_by_type_[plugin_type] = plugin;

        plugins_.push_back({std::move(library), std::unique_ptr<common::TaskPlugin, PluginDeleter>(
                                                    plugin, PluginDeleter{destroy})});
        LOG_INFO("plugin loaded: " << name << " plugin_type=" << plugin->pluginType());
    }
    return true;
}

}  // namespace scheduler
