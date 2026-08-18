#include "scheduler/Scheduler.h"

#include "common/Logger.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scheduler {

namespace {

class ProcessResultEvent : public common::Event {
public:
    explicit ProcessResultEvent(common::TaskPlugin* plugin, common::TaskResult result)
        : plugin_(plugin), result_(std::move(result)) {
    }

    void execute() override {
        try {
            plugin_->processResult(result_);
        } catch (const std::exception& e) {
            LOG_ERROR("plugin " << plugin_->pluginType() << ": processResult threw: " << e.what());
        } catch (...) {
            LOG_ERROR("plugin " << plugin_->pluginType()
                               << ": processResult threw unknown exception");
        }
    }

private:
    common::TaskPlugin* plugin_;
    common::TaskResult result_;
};

std::string GenerateRunId() {
    const auto now = std::chrono::system_clock::now();
    return "run-" + std::to_string(now.time_since_epoch().count());
}

}  // namespace

Scheduler::Scheduler(common::Config& config)
    : config_(config),
      server_(config.get("server.router_endpoint", std::string("tcp://*:5555")),
              std::chrono::milliseconds(config.get("server.poll_timeout_ms", 50))) {
}

Scheduler::~Scheduler() {
    stop();
}

void Scheduler::start() {
    if (!LoadPlugins()) {
        throw std::runtime_error("Scheduler: plugin loading failed");
    }
    const std::string db_conn = config_.get("server.db_conn", std::string());
    if (!db_conn.empty()) {
        if (!storage_.init(db_conn)) {
            throw std::runtime_error("Scheduler: storage init failed");
        }
        // Try to resume runs left running/stopped by a previous process;
        // runs that cannot be restored are marked interrupted.
        std::unordered_set<std::string> restored_plugins;
        for (const Storage::RunRecord& run : storage_.LoadUnfinishedRuns()) {
            const auto pit = plugins_by_type_.find(run.plugin_type);
            if (pit == plugins_by_type_.end()) {
                LOG_WARN("storage: run " << run.run_id << " plugin_type=" << run.plugin_type
                                         << " has no loaded plugin, marked interrupted");
                storage_.FinishRun(run.run_id, "interrupted");
                continue;
            }
            if (!restored_plugins.insert(run.plugin_type).second) {
                LOG_WARN("storage: run " << run.run_id << " skipped, plugin "
                                         << run.plugin_type << " already restored");
                storage_.FinishRun(run.run_id, "interrupted");
                continue;
            }
            bool restored = false;
            try {
                restored = pit->second->restore(run.run_id);
            } catch (const std::exception& e) {
                LOG_ERROR("plugin " << run.plugin_type << ": restore threw: " << e.what());
            } catch (...) {
                LOG_ERROR("plugin " << run.plugin_type
                                    << ": restore threw unknown exception");
            }
            if (restored) {
                storage_.ResumeRun(run.run_id);
                active_runs_[pit->second] = {run.run_id, run.plugin_type};
                active_run_count_.fetch_add(1, std::memory_order_relaxed);
                LOG_INFO("run restored: " << run.run_id
                                          << " plugin_type=" << run.plugin_type);
            } else {
                LOG_WARN("storage: run " << run.run_id
                                         << " cannot be restored, marked interrupted");
                storage_.FinishRun(run.run_id, "interrupted");
            }
        }
    }
    server_.start();
    const int process_threads = config_.get("server.process_threads", 4);
    event_manager_ = std::make_unique<common::EventManager>(process_threads);
    running_.store(true);
    processing_ = std::thread(&Scheduler::RunProcessing, this);
}

void Scheduler::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    server_.stop();
    if (processing_.joinable()) {
        processing_.join();
    }
    // Join result-processing workers. Events still queued are dropped by
    // design: a stopped scheduler means "work resumes next run".
    event_manager_.reset();
    plugins_.clear();
}

bool Scheduler::createRun(const std::string& plugin_type, const std::string& dataset_version,
                          const std::string& params) {
    commands_.push({plugin_type, dataset_version, params});
    return true;
}

bool Scheduler::stopRun(const std::string& plugin_type) {
    stop_commands_.push({plugin_type});
    return true;
}

void Scheduler::wakeupClients(const std::string& plugin_type) {
    std::vector<std::string> identities;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (const auto& entry : clients_) {
            if (plugin_type.empty() || entry.second.info.plugin_type == plugin_type) {
                identities.push_back(entry.first);
            }
        }
    }
    common::Envelope env;
    env.set_type(common::WAKE);
    for (const std::string& identity : identities) {
        server_.sendTo(identity, env);
        LOG_INFO("wakeup sent to " << identity);
    }
}

std::vector<common::ClientInfo> Scheduler::clients() const {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    std::vector<common::ClientInfo> out;
    out.reserve(clients_.size());
    const auto now = std::chrono::steady_clock::now();
    for (const auto& entry : clients_) {
        common::ClientInfo info = entry.second.info;
        info.last_seen_age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    now - entry.second.last_seen)
                                    .count();
        info.registered = entry.second.registered;
        out.push_back(std::move(info));
    }
    return out;
}

size_t Scheduler::pluginCount() const {
    return plugins_by_type_.size();
}

size_t Scheduler::inFlightCount() const {
    std::lock_guard<std::mutex> lock(in_flight_mutex_);
    return in_flight_.size();
}

std::vector<std::string> Scheduler::pluginTypes() const {
    std::vector<std::string> out;
    for (const auto& entry : plugins_by_type_) {
        out.push_back(entry.first);
    }
    return out;
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

        plugins_by_type_[plugin->pluginType()] = plugin;
        plugins_.push_back(
            {std::move(library),
             std::unique_ptr<common::TaskPlugin, PluginDeleter>(plugin, PluginDeleter{destroy})});
        LOG_INFO("plugin loaded: " << name << " plugin_type=" << plugin->pluginType());
    }
    return true;
}

void Scheduler::RunProcessing() {
    auto last_check = std::chrono::steady_clock::now();
    while (running_.load(std::memory_order_relaxed)) {
        IncomingMessage msg;
        if (server_.tryRecv(msg, std::chrono::milliseconds(100))) {
            HandleMessage(msg);
        }

        CreateRunCommand cmd;
        while (commands_.pop(cmd, std::chrono::milliseconds(0))) {
            HandleCreateRun(cmd);
        }

        StopRunCommand stop_cmd;
        while (stop_commands_.pop(stop_cmd, std::chrono::milliseconds(0))) {
            HandleStopRun(stop_cmd);
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_check >= std::chrono::milliseconds(500)) {
            last_check = now;
            RunTimeoutChecks();
        }
    }
}

void Scheduler::HandleMessage(const IncomingMessage& msg) {
    const common::Envelope& env = msg.envelope;
    if (!msg.peer_address.empty()) {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_[msg.identity].info.peer_address = msg.peer_address;
    }
    switch (env.type()) {
        case common::REGISTER:
            if (env.has_register_()) {
                HandleRegister(msg);
            }
            break;
        case common::HEARTBEAT:
            if (env.has_heartbeat()) {
                UpdateClient(msg.identity, env.plugin_type(), env.heartbeat());
            }
            break;
        case common::REQUEST_TASK:
            if (env.has_request_task()) {
                HandleRequestTask(msg);
            }
            break;
        case common::TASK_REPORT:
            if (env.has_task_report()) {
                HandleTaskReport(msg);
            }
            break;
        case common::TASK_ACK:
            if (env.has_task_ack()) {
                HandleTaskAck(msg);
            }
            break;
        case common::TASK_REJECT:
            if (env.has_task_reject()) {
                HandleTaskReject(msg);
            }
            break;
        case common::REGISTER_RESULT:
        case common::TASK:
        case common::NO_TASK:
        case common::WAKE:
        case common::STOP:
            LOG_WARN("unexpected message type " << env.type() << " from " << msg.identity);
            break;
        default:
            LOG_WARN("unhandled message type " << env.type() << " from " << msg.identity);
            break;
    }
}

void Scheduler::HandleRegister(const IncomingMessage& msg) {
    const common::RegisterCommand& reg = msg.envelope.register_();
    const std::string& plugin_type = msg.envelope.plugin_type();
    const auto it = plugins_by_type_.find(plugin_type);
    if (it == plugins_by_type_.end()) {
        LOG_ERROR("register: no plugin for plugin_type " << plugin_type);
        SendRegisterResult(msg.identity, plugin_type, false, false,
                           "no plugin for plugin_type " + plugin_type, std::string());
        return;
    }

    common::TaskPlugin* plugin = it->second;
    common::ClientInfo client;
    client.identity = msg.identity;
    client.plugin_type = plugin_type;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        const auto cit = clients_.find(msg.identity);
        if (cit != clients_.end()) {
            client.heartbeat = cit->second.info.heartbeat;
        }
    }

    common::ValidationResult result;
    try {
        if (!plugin->validateClient(client, reg.payload(), result)) {
            LOG_ERROR("plugin " << plugin_type << ": validateClient failed");
            SendRegisterResult(msg.identity, plugin_type, true, false, result.reason,
                               result.reply_payload);
            return;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("plugin " << plugin_type << ": validateClient threw: " << e.what());
        SendRegisterResult(msg.identity, plugin_type, true, false, "validateClient threw",
                           std::string());
        return;
    } catch (...) {
        LOG_ERROR("plugin " << plugin_type << ": validateClient threw unknown exception");
        SendRegisterResult(msg.identity, plugin_type, true, false,
                           "validateClient threw unknown exception", std::string());
        return;
    }

    TouchClient(msg.identity, plugin_type);
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_[msg.identity].registered = result.accepted;
    }
    SendRegisterResult(msg.identity, plugin_type, true, result.accepted, result.reason,
                       result.reply_payload);
    LOG_INFO("register: client " << msg.identity << " plugin_type=" << plugin_type
                                 << (result.accepted ? " accepted" : " rejected"));
}

void Scheduler::HandleRequestTask(const IncomingMessage& msg) {
    const common::RequestTaskCommand& request = msg.envelope.request_task();
    const std::string& plugin_type = msg.envelope.plugin_type();
    const auto it = plugins_by_type_.find(plugin_type);
    if (it == plugins_by_type_.end()) {
        LOG_WARN("request_task: no plugin for plugin_type " << plugin_type
                                                            << " from " << msg.identity);
        SendNoTask(msg.identity, plugin_type);
        return;
    }

    TouchClient(msg.identity, plugin_type);

    // Only registered clients may pull tasks.
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        const auto cit = clients_.find(msg.identity);
        if (cit == clients_.end() || !cit->second.registered) {
            LOG_WARN("request_task: unregistered client " << msg.identity
                                                          << ", no task granted");
            SendNoTask(msg.identity, plugin_type);
            return;
        }
    }

    common::TaskPlugin* plugin = it->second;
    common::ClientInfo client;
    client.identity = msg.identity;
    client.plugin_type = plugin_type;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        const auto cit = clients_.find(msg.identity);
        if (cit != clients_.end()) {
            client.heartbeat = cit->second.info.heartbeat;
        }
    }

    const int count = request.count();
    if (count <= 0) {
        LOG_WARN("request_task: invalid count " << count << " from " << msg.identity);
        SendNoTask(msg.identity, plugin_type);
        return;
    }

    std::vector<common::Task> tasks;
    tasks.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        common::Task task;
        bool has_task = false;
        try {
            has_task = plugin->requestTask(client, request.payload(), task);
        } catch (const std::exception& e) {
            LOG_ERROR("plugin " << plugin_type << ": requestTask threw: " << e.what());
            break;
        } catch (...) {
            LOG_ERROR("plugin " << plugin_type << ": requestTask threw unknown exception");
            break;
        }
        if (!has_task) {
            break;
        }
        tasks.push_back(std::move(task));
    }

    if (tasks.empty()) {
        SendNoTask(msg.identity, plugin_type);
    } else {
        SendTasks(plugin, msg.identity, tasks);
    }
}

void Scheduler::HandleTaskReport(const IncomingMessage& msg) {
    const common::TaskReport& report = msg.envelope.task_report();
    const std::string& plugin_type = msg.envelope.plugin_type();
    const auto pit = plugins_by_type_.find(plugin_type);
    if (pit == plugins_by_type_.end()) {
        LOG_ERROR("task_report: no plugin for plugin_type " << plugin_type);
        return;
    }

    TouchClient(msg.identity, plugin_type);

    {
        std::lock_guard<std::mutex> lock(in_flight_mutex_);
        const auto it = in_flight_.find(report.result().task_id());
        if (it == in_flight_.end()) {
            LOG_WARN("completion for unknown task " << report.result().task_id()
                                                    << " from " << msg.identity << ", dropped");
            return;
        }
        if (it->second.identity != msg.identity) {
            LOG_WARN("completion for task " << report.result().task_id() << " from "
                                            << msg.identity << " (assigned to "
                                            << it->second.identity << "), dropped");
            return;
        }
        in_flight_.erase(it);
    }

    if (event_manager_ == nullptr) {
        LOG_ERROR("result processing pool is not running");
        return;
    }
    if (storage_.enabled()) {
        storage_.ReportTask(report.result().task_id(), report.result());
    }
    event_manager_->push(new ProcessResultEvent(pit->second, report.result()));
}

void Scheduler::HandleTaskAck(const IncomingMessage& msg) {
    const common::TaskAck& ack = msg.envelope.task_ack();
    TouchClient(msg.identity, msg.envelope.plugin_type());

    std::lock_guard<std::mutex> lock(in_flight_mutex_);
    const auto it = in_flight_.find(ack.task_id());
    if (it == in_flight_.end()) {
        LOG_WARN("ack for unknown task " << ack.task_id() << " from " << msg.identity);
        return;
    }
    it->second.acked = true;
    if (storage_.enabled()) {
        storage_.AckTask(ack.task_id());
    }
    LOG_INFO("task " << ack.task_id() << " acked by " << msg.identity);
}

void Scheduler::HandleCreateRun(const CreateRunCommand& command) {
    const auto it = plugins_by_type_.find(command.plugin_type);
    if (it == plugins_by_type_.end()) {
        LOG_ERROR("createRun: no plugin for plugin_type " << command.plugin_type);
        return;
    }

    common::RunRequest request;
    request.run_id = GenerateRunId();
    request.dataset_version = command.dataset_version;
    request.params = command.params;

    common::TaskPlugin* plugin = it->second;
    if (active_runs_.count(plugin) > 0) {
        bool finished = false;
        try {
            finished = plugin->runFinished();
        } catch (const std::exception& e) {
            LOG_ERROR("plugin " << command.plugin_type
                                << ": runFinished threw: " << e.what());
        } catch (...) {
            LOG_ERROR("plugin " << command.plugin_type
                                << ": runFinished threw unknown exception");
        }
        if (!finished) {
            LOG_ERROR("createRun: run already active for plugin_type " << command.plugin_type);
            return;
        }
    }
    try {
        if (!plugin->plan(request, SnapshotClients())) {
            LOG_ERROR("createRun: plan failed for " << command.plugin_type);
            return;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("createRun: plugin " << command.plugin_type << " plan threw: " << e.what());
        return;
    } catch (...) {
        LOG_ERROR("createRun: plugin " << command.plugin_type
                                       << " plan threw unknown exception");
        return;
    }

    active_runs_[plugin] = {request.run_id, command.plugin_type};
    active_run_count_.fetch_add(1, std::memory_order_relaxed);
    if (storage_.enabled()) {
        storage_.SaveRun(request.run_id, command.plugin_type, request.dataset_version,
                         request.params);
    }
    LOG_INFO("run started: " << request.run_id << " plugin_type=" << command.plugin_type);
}

void Scheduler::HandleStopRun(const StopRunCommand& command) {
    const auto it = plugins_by_type_.find(command.plugin_type);
    if (it == plugins_by_type_.end()) {
        LOG_ERROR("stop: no plugin for plugin_type " << command.plugin_type);
        return;
    }

    common::TaskPlugin* plugin = it->second;
    const auto run_it = active_runs_.find(plugin);
    if (run_it == active_runs_.end()) {
        LOG_WARN("stop: no active run for plugin_type " << command.plugin_type);
        return;
    }
    const std::string run_id = run_it->second.run_id;

    // Send STOP to every client of this plugin type and reclaim in-flight tasks.
    std::vector<std::string> clients_of_type;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (const auto& entry : clients_) {
            if (entry.second.info.plugin_type == command.plugin_type) {
                clients_of_type.push_back(entry.first);
            }
        }
    }

    common::Envelope env;
    env.set_type(common::STOP);
    env.set_plugin_type(command.plugin_type);
    env.mutable_stop()->set_run_id(run_id);
    env.mutable_stop()->set_reason("manual stop");
    for (const std::string& identity : clients_of_type) {
        ReclaimClientTasks(identity);
        server_.sendTo(identity, env);
        LOG_INFO("stop sent to client " << identity);
    }

    try {
        if (!plugin->saveCheckpoint(run_id)) {
            LOG_ERROR("plugin " << command.plugin_type << ": saveCheckpoint failed");
        }
    } catch (const std::exception& e) {
        LOG_ERROR("plugin " << command.plugin_type << ": saveCheckpoint threw: " << e.what());
    } catch (...) {
        LOG_ERROR("plugin " << command.plugin_type
                            << ": saveCheckpoint threw unknown exception");
    }

    try {
        plugin->cancel();
    } catch (const std::exception& e) {
        LOG_ERROR("plugin " << command.plugin_type << ": cancel threw: " << e.what());
    } catch (...) {
        LOG_ERROR("plugin " << command.plugin_type << ": cancel threw unknown exception");
    }

    active_runs_.erase(run_it);
    active_run_count_.fetch_sub(1, std::memory_order_relaxed);
    if (storage_.enabled()) {
        storage_.FinishRun(run_id, "stopped");
    }
    LOG_INFO("run stopped: " << run_id << " plugin_type=" << command.plugin_type);
}

void Scheduler::ReclaimClientTasks(const std::string& identity) {
    std::unordered_map<std::string, std::vector<std::string>> by_type;
    std::vector<std::string> all_ids;
    {
        std::lock_guard<std::mutex> lock(in_flight_mutex_);
        for (auto it = in_flight_.begin(); it != in_flight_.end();) {
            if (it->second.identity == identity) {
                by_type[it->second.plugin_type].push_back(it->first);
                all_ids.push_back(it->first);
                it = in_flight_.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (storage_.enabled()) {
        storage_.ReclaimTasks(all_ids);
    }
    for (const auto& item : by_type) {
        const auto pit = plugins_by_type_.find(item.first);
        if (pit != plugins_by_type_.end()) {
            LOG_WARN("reclaim " << item.second.size() << " tasks of client " << identity
                                << " plugin_type=" << item.first);
            try {
                pit->second->requeue(item.second);
            } catch (const std::exception& e) {
                LOG_ERROR("plugin " << item.first << ": requeue threw: " << e.what());
            } catch (...) {
                LOG_ERROR("plugin " << item.first << ": requeue threw unknown exception");
            }
        }
    }
}

void Scheduler::TouchClient(const std::string& identity, const std::string& plugin_type) {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    const bool is_new = clients_.find(identity) == clients_.end();
    ClientEntry& entry = clients_[identity];
    entry.info.plugin_type = plugin_type;
    entry.last_seen = std::chrono::steady_clock::now();
    if (is_new) {
        LOG_INFO("client online: " << identity);
    }
}

bool Scheduler::UpdateClient(const std::string& identity, const std::string& plugin_type,
                             const common::Heartbeat& heartbeat) {
    std::lock_guard<std::mutex> lock(clients_mutex_);

    const auto it = clients_.find(identity);
    bool changed = it == clients_.end();
    if (!changed) {
        const common::Heartbeat& old = it->second.info.heartbeat;
        changed = old.status() != heartbeat.status() ||
                  old.ip() != heartbeat.ip();
    }

    ClientEntry& entry = clients_[identity];
    entry.info.identity = identity;
    entry.info.plugin_type = plugin_type;
    entry.info.heartbeat = heartbeat;
    entry.last_seen = std::chrono::steady_clock::now();
    return changed;
}

std::vector<common::ClientInfo> Scheduler::SnapshotClients() const {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    std::vector<common::ClientInfo> snapshot;
    snapshot.reserve(clients_.size());
    for (const auto& entry : clients_) {
        snapshot.push_back(entry.second.info);
    }
    return snapshot;
}

void Scheduler::RunTimeoutChecks() {
    // Clear finished run contexts so new runs can start.
    for (auto it = active_runs_.begin(); it != active_runs_.end();) {
        bool finished = false;
        try {
            finished = it->first->runFinished();
        } catch (const std::exception& e) {
            LOG_ERROR("plugin " << it->second.plugin_type
                                << ": runFinished threw: " << e.what());
        } catch (...) {
            LOG_ERROR("plugin " << it->second.plugin_type
                                << ": runFinished threw unknown exception");
        }
        if (finished) {
            LOG_INFO("run finished: " << it->second.run_id
                                      << " plugin_type=" << it->second.plugin_type);
            if (storage_.enabled()) {
                storage_.FinishRun(it->second.run_id, "finished");
            }
            it = active_runs_.erase(it);
            active_run_count_.fetch_sub(1, std::memory_order_relaxed);
        } else {
            ++it;
        }
    }

    // Conservative by default: the framework does not know how long a plugin
    // task may legitimately run (e.g. coverage on large stp files), so both
    // recovery timeouts default to disabled (0). Users opt in based on their
    // own task duration expectations.
    const int client_timeout_ms = config_.get("server.client_timeout_ms", 0);
    const int lease_timeout_ms = config_.get("server.task_lease_timeout_ms", 0);
    const auto timeout = std::chrono::milliseconds(client_timeout_ms);
    const auto lease = std::chrono::milliseconds(lease_timeout_ms);
    const auto now = std::chrono::steady_clock::now();

    std::vector<std::string> offline;
    if (client_timeout_ms > 0) {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (auto it = clients_.begin(); it != clients_.end();) {
            if (now - it->second.last_seen > timeout) {
                offline.push_back(it->first);
                it = clients_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const std::string& identity : offline) {
        LOG_WARN("client offline: " << identity);
        ReclaimClientTasks(identity);
    }

    if (lease_timeout_ms > 0) {
        std::unordered_map<std::string, std::vector<std::string>> by_type;
        std::vector<std::string> lease_ids;
        {
            std::lock_guard<std::mutex> lock(in_flight_mutex_);
            for (auto it = in_flight_.begin(); it != in_flight_.end();) {
                if (now - it->second.sent_at > lease) {
                    LOG_WARN("reclaim task " << it->first << ": lease expired"
                                             << (it->second.acked ? " (acked)"
                                                                  : " (not acked)"));
                    by_type[it->second.plugin_type].push_back(it->first);
                    lease_ids.push_back(it->first);
                    it = in_flight_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        if (storage_.enabled()) {
            storage_.ReclaimTasks(lease_ids);
        }
        for (const auto& item : by_type) {
            const auto pit = plugins_by_type_.find(item.first);
            if (pit != plugins_by_type_.end()) {
                LOG_WARN("requeue " << item.second.size() << " tasks of plugin " << item.first);
                try {
                    pit->second->requeue(item.second);
                } catch (const std::exception& e) {
                    LOG_ERROR("plugin " << item.first << ": requeue threw: " << e.what());
                } catch (...) {
                    LOG_ERROR("plugin " << item.first << ": requeue threw unknown exception");
                }
            }
        }
    }
}

void Scheduler::SendRegisterResult(const std::string& identity, const std::string& plugin_type,
                                   bool plugin_found, bool accepted, const std::string& reason,
                                   const std::string& reply_payload) {
    common::Envelope env;
    env.set_type(common::REGISTER_RESULT);
    env.set_plugin_type(plugin_type);
    common::RegisterResult* result = env.mutable_register_result();
    result->set_plugin_found(plugin_found);
    result->set_accepted(accepted);
    result->set_reason(reason);
    result->set_payload(reply_payload);
    server_.sendTo(identity, env);
}

void Scheduler::HandleTaskReject(const IncomingMessage& msg) {
    const common::TaskRejectCommand& reject = msg.envelope.task_reject();
    TouchClient(msg.identity, msg.envelope.plugin_type());

    std::string plugin_type;
    std::vector<std::string> task_ids;
    {
        std::lock_guard<std::mutex> lock(in_flight_mutex_);
        const auto it = in_flight_.find(reject.task_id());
        if (it == in_flight_.end()) {
            LOG_WARN("reject for unknown task " << reject.task_id() << " from "
                                                << msg.identity);
            return;
        }
        if (it->second.identity != msg.identity) {
            LOG_WARN("reject for task " << reject.task_id() << " from " << msg.identity
                                        << " (assigned to " << it->second.identity
                                        << "), dropped");
            return;
        }
        plugin_type = it->second.plugin_type;
        task_ids.push_back(it->first);
        in_flight_.erase(it);
    }

    if (storage_.enabled()) {
        storage_.ReclaimTasks(task_ids);
    }
    const auto pit = plugins_by_type_.find(plugin_type);
    if (pit != plugins_by_type_.end()) {
        LOG_WARN("task rejected by client " << msg.identity << ": " << reject.task_id()
                                            << " reason=" << reject.reason()
                                            << ", requeue to plugin " << plugin_type);
        try {
            pit->second->requeue(task_ids);
        } catch (const std::exception& e) {
            LOG_ERROR("plugin " << plugin_type << ": requeue threw: " << e.what());
        } catch (...) {
            LOG_ERROR("plugin " << plugin_type << ": requeue threw unknown exception");
        }
    }
}

void Scheduler::SendTasks(common::TaskPlugin* plugin, const std::string& identity,
                          const std::vector<common::Task>& tasks) {
    const auto it = active_runs_.find(plugin);
    if (it == active_runs_.end()) {
        LOG_ERROR("no active run for plugin " << plugin->pluginType() << ", returning "
                                              << tasks.size() << " tasks to the plugin");
        std::vector<std::string> task_ids;
        task_ids.reserve(tasks.size());
        for (const common::Task& task : tasks) {
            task_ids.push_back(task.task_id());
        }
        try {
            plugin->requeue(task_ids);
        } catch (const std::exception& e) {
            LOG_ERROR("plugin " << plugin->pluginType()
                                << ": requeue on send failure threw: " << e.what());
        } catch (...) {
            LOG_ERROR("plugin " << plugin->pluginType()
                                << ": requeue on send failure threw unknown exception");
        }
        return;
    }
    const RunContext& run = it->second;

    common::Envelope env;
    env.set_type(common::TASK);
    env.set_plugin_type(run.plugin_type);
    common::TaskCommand* cmd = env.mutable_task();
    cmd->set_run_id(run.run_id);

    std::vector<common::Task> accepted;
    accepted.reserve(tasks.size());
    {
        std::lock_guard<std::mutex> lock(in_flight_mutex_);
        for (const common::Task& task : tasks) {
            if (in_flight_.count(task.task_id()) > 0) {
                LOG_ERROR("task " << task.task_id()
                                  << " already in flight, duplicate dispatch dropped");
                continue;
            }
            *cmd->add_tasks() = task;
            in_flight_[task.task_id()] = {identity, run.plugin_type,
                                          std::chrono::steady_clock::now(), false};
            accepted.push_back(task);
        }
    }
    if (accepted.empty()) {
        return;
    }

    server_.sendTo(identity, env);
    if (storage_.enabled()) {
        for (const common::Task& task : accepted) {
            storage_.SaveTask(task.task_id(), run.run_id, identity, task.payload());
        }
    }
    for (const common::Task& task : accepted) {
        LOG_INFO("task " << task.task_id() << " -> client " << identity);
    }
}

void Scheduler::SendNoTask(const std::string& identity, const std::string& plugin_type) {
    common::Envelope env;
    env.set_type(common::NO_TASK);
    env.set_plugin_type(plugin_type);
    server_.sendTo(identity, env);
}

}  // namespace scheduler
