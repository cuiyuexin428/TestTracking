#include "scheduler/Scheduler.h"

#include "common/Logger.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace scheduler {

namespace {

class ProcessResultEvent : public common::Event {
public:
    ProcessResultEvent(common::TaskPlugin* plugin, common::TaskResult result,
                       std::function<void()> on_done)
        : plugin_(plugin), result_(std::move(result)), on_done_(std::move(on_done)) {
    }

    void execute() override {
        try {
            plugin_->processResult(result_);
        } catch (const std::exception& e) {
            LOG_ERROR("plugin " << plugin_->taskType() << ": processResult threw: " << e.what());
        } catch (...) {
            LOG_ERROR("plugin " << plugin_->taskType()
                               << ": processResult threw unknown exception");
        }
        try {
            if (on_done_) {
                on_done_();
            }
        } catch (const std::exception& e) {
            LOG_ERROR("dispatch trigger failed: " << e.what());
        } catch (...) {
            LOG_ERROR("dispatch trigger failed: unknown exception");
        }
    }

private:
    common::TaskPlugin* plugin_;
    common::TaskResult result_;
    std::function<void()> on_done_;
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

bool Scheduler::createRun(const std::string& task_type, const std::string& dataset_version,
                          const std::string& params) {
    commands_.push({task_type, dataset_version, params});
    return true;
}

std::vector<common::ClientInfo> Scheduler::clients() const {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    std::vector<common::ClientInfo> out;
    out.reserve(clients_.size());
    for (const auto& entry : clients_) {
        out.push_back(entry.second.info);
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

        plugins_by_type_[plugin->taskType()] = plugin;
        plugins_.push_back(
            {std::move(library),
             std::unique_ptr<common::TaskPlugin, PluginDeleter>(plugin, PluginDeleter{destroy})});
        LOG_INFO("plugin loaded: " << name << " task_type=" << plugin->taskType());
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

            int trigger = 0;
            while (dispatch_triggers_.pop(trigger, std::chrono::milliseconds(0))) {
                TriggerDispatch();
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
    switch (env.type()) {
        case common::HEARTBEAT:
            if (env.has_heartbeat() && UpdateClient(msg.identity, env.heartbeat())) {
                TriggerDispatch();
            }
            break;
        case common::RESPONSE:
            if (env.has_response()) {
                HandleResponse(env.response());
            }
            break;
        case common::ACK:
            LOG_INFO("ack from " << msg.identity << ": " << env.request_id());
            break;
        default:
            LOG_WARN("unhandled message type " << env.type() << " from " << msg.identity);
            break;
    }
}

void Scheduler::HandleCreateRun(const CreateRunCommand& command) {
    const auto it = plugins_by_type_.find(command.task_type);
    if (it == plugins_by_type_.end()) {
        LOG_ERROR("createRun: no plugin for task_type " << command.task_type);
        return;
    }

    common::RunRequest request;
    request.run_id = GenerateRunId();
    request.dataset_version = command.dataset_version;
    request.params = command.params;

    common::TaskPlugin* plugin = it->second;
    try {
        if (!plugin->plan(request, SnapshotClients())) {
            LOG_ERROR("createRun: plan failed for " << command.task_type);
            return;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("createRun: plugin " << command.task_type << " plan threw: " << e.what());
        return;
    } catch (...) {
        LOG_ERROR("createRun: plugin " << command.task_type << " plan threw unknown exception");
        return;
    }

    active_runs_[plugin] = {request.run_id, command.task_type, 0};
    LOG_INFO("run started: " << request.run_id << " task_type=" << command.task_type);
    TriggerDispatch();
}

void Scheduler::HandleResponse(const common::Response& response) {
    if (!response.has_run_wave()) {
        LOG_WARN("response without run_wave result, request_id=" << response.request_id());
        return;
    }

    const common::RunWaveResult& result = response.run_wave();
    for (common::TaskResult task : result.tasks()) {
        InFlight in_flight;
        {
            std::lock_guard<std::mutex> lock(in_flight_mutex_);
            const auto it = in_flight_.find(task.task_id());
            if (it == in_flight_.end()) {
                LOG_WARN("completion for unknown task " << task.task_id());
                continue;
            }
            in_flight = it->second;
            in_flight_.erase(it);
        }

        const auto pit = plugins_by_type_.find(in_flight.task_type);
        if (pit == plugins_by_type_.end()) {
            LOG_ERROR("completion for unknown plugin " << in_flight.task_type);
            continue;
        }
        if (event_manager_ == nullptr) {
            LOG_ERROR("result processing pool is not running");
            continue;
        }
        event_manager_->push(new ProcessResultEvent(
            pit->second, std::move(task), [this] { dispatch_triggers_.push(0); }));
    }
}

bool Scheduler::UpdateClient(const std::string& identity, const common::Heartbeat& heartbeat) {
    std::lock_guard<std::mutex> lock(clients_mutex_);

    const auto it = clients_.find(identity);
    bool changed = it == clients_.end();
    if (!changed) {
        const common::Heartbeat& old = it->second.info.heartbeat;
        changed = old.status() != heartbeat.status() ||
                  old.dataset_version() != heartbeat.dataset_version() ||
                  old.dataset_ready() != heartbeat.dataset_ready() ||
                  old.program_version() != heartbeat.program_version();
    }

    ClientEntry& entry = clients_[identity];
    entry.info.identity = identity;
    entry.info.heartbeat = heartbeat;
    entry.last_seen = std::chrono::steady_clock::now();

    if (it == clients_.end()) {
        LOG_INFO("client online: " << identity << " worker=" << heartbeat.worker_id());
    }
    return changed;
}

std::vector<common::ClientInfo> Scheduler::SnapshotClients() const {
    const std::string expected_version = config_.get("server.program_version", std::string());

    std::lock_guard<std::mutex> lock(clients_mutex_);
    std::vector<common::ClientInfo> snapshot;
    snapshot.reserve(clients_.size());
    for (const auto& entry : clients_) {
        if (!expected_version.empty() &&
            entry.second.info.heartbeat.program_version() != expected_version) {
            continue;
        }
        snapshot.push_back(entry.second.info);
    }
    return snapshot;
}

void Scheduler::RunTimeoutChecks() {
    const int client_timeout_ms = config_.get("server.client_timeout_ms", 3000);
    const auto timeout = std::chrono::milliseconds(client_timeout_ms);
    const auto now = std::chrono::steady_clock::now();

    std::vector<std::string> offline;
    {
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
    if (offline.empty()) {
        return;
    }

    std::unordered_map<std::string, std::vector<std::string>> by_type;
    {
        std::lock_guard<std::mutex> lock(in_flight_mutex_);
        for (auto it = in_flight_.begin(); it != in_flight_.end();) {
            if (std::find(offline.begin(), offline.end(), it->second.identity) != offline.end()) {
                by_type[it->second.task_type].push_back(it->first);
                it = in_flight_.erase(it);
            } else {
                ++it;
            }
        }
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
    for (const std::string& identity : offline) {
        LOG_WARN("client offline: " << identity);
    }
    TriggerDispatch();
}

void Scheduler::TriggerDispatch() {
    for (const auto& item : plugins_by_type_) {
        std::vector<common::TaskAssignment> out;
        try {
            if (!item.second->dispatch(SnapshotClients(), out)) {
                LOG_ERROR("plugin " << item.first << ": dispatch failed");
                continue;
            }
        } catch (const std::exception& e) {
            LOG_ERROR("plugin " << item.first << ": dispatch threw: " << e.what());
            continue;
        } catch (...) {
            LOG_ERROR("plugin " << item.first << ": dispatch threw unknown exception");
            continue;
        }
        for (const common::TaskAssignment& assignment : out) {
            SendRunWave(item.second, assignment);
        }
    }
}

void Scheduler::SendRunWave(common::TaskPlugin* plugin, const common::TaskAssignment& assignment) {
    const auto it = active_runs_.find(plugin);
    if (it == active_runs_.end()) {
        LOG_ERROR("no active run for plugin " << plugin->taskType());
        return;
    }
    RunContext run = it->second;
    ++it->second.wave;

    common::Envelope env;
    env.set_type(common::RUN_WAVE);
    env.set_request_id(assignment.task.task_id());
    common::RunWaveCommand* cmd = env.mutable_run_wave();
    cmd->set_run_id(run.run_id);
    cmd->set_wave_id("wave-" + std::to_string(run.wave));
    cmd->set_task_type(run.task_type);
    *cmd->add_tasks() = assignment.task;

    server_.sendTo(assignment.client_identity, env);
    {
        std::lock_guard<std::mutex> lock(in_flight_mutex_);
        in_flight_[assignment.task.task_id()] = {assignment.client_identity, run.task_type};
    }
    LOG_INFO("task " << assignment.task.task_id() << " -> client " << assignment.client_identity);
}

}  // namespace scheduler
