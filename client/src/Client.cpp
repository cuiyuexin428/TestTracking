#include "client/Client.h"

#include "common/Logger.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace client {

Client::Client(common::Config& config)
    : config_(config),
      dealer_(config.get("scheduler.endpoint", std::string("tcp://127.0.0.1:5555"))),
      worker_id_(config.get("client.worker_id", std::string("worker-1"))),
      program_version_(config.get("client.program_version", std::string())),
      dataset_version_(config.get("client.dataset_version", std::string())),
      dataset_ready_(config.get("client.dataset_ready", false)),
      heartbeat_interval_(config.get("client.heartbeat_interval_ms", 1000)),
      poll_timeout_(config.get("scheduler.poll_timeout_ms", 50)) {
}

Client::~Client() {
    stop();
}

void Client::start() {
    if (!LoadPlugins()) {
        throw std::runtime_error("Client: plugin loading failed");
    }
    dealer_.setOption(ZMQ_IDENTITY, worker_id_.data(), worker_id_.size());
    const int recv_timeout_ms = static_cast<int>(poll_timeout_.count());
    dealer_.setOption(ZMQ_RCVTIMEO, &recv_timeout_ms, sizeof(recv_timeout_ms));
    dealer_.connect();
    running_.store(true);
    poll_thread_ = std::thread(&Client::RunPoll, this);
    executor_thread_ = std::thread(&Client::RunExecutor, this);
}

void Client::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (poll_thread_.joinable()) {
        poll_thread_.join();
    }
    if (executor_thread_.joinable()) {
        executor_thread_.join();
    }
    plugins_.clear();
}

bool Client::LoadPlugins() {
    const nlohmann::json* plugins_node = config_.find("plugins");
    if (plugins_node == nullptr || !plugins_node->is_object()) {
        LOG_WARN("no plugins configured; heartbeats only");
        return true;
    }

    for (auto it = plugins_node->begin(); it != plugins_node->end(); ++it) {
        const std::string& name = it.key();
        const nlohmann::json& entry = it.value();
        if (!entry.value("enabled", false)) {
            continue;
        }

        const std::string dll_path = entry.value("dll", std::string());
        std::unique_ptr<common::DynamicLibrary> library = common::DynamicLibrary::load(dll_path);
        if (library == nullptr) {
            LOG_ERROR("client plugin " << name << ": cannot load " << dll_path << ": "
                                       << common::DynamicLibrary::lastError());
            return false;
        }

        auto create = library->get<common::CreateClientPluginFn>("CreateClientPlugin");
        auto destroy = library->get<common::DestroyClientPluginFn>("DestroyClientPlugin");
        if (create == nullptr || destroy == nullptr) {
            LOG_ERROR("client plugin " << name
                                       << ": missing CreateClientPlugin/DestroyClientPlugin");
            return false;
        }

        common::ClientPlugin* plugin = nullptr;
        try {
            plugin = create();
        } catch (const std::exception& e) {
            LOG_ERROR("client plugin " << name << ": CreateClientPlugin threw: " << e.what());
            return false;
        } catch (...) {
            LOG_ERROR("client plugin " << name << ": CreateClientPlugin threw unknown exception");
            return false;
        }
        if (plugin == nullptr) {
            LOG_ERROR("client plugin " << name << ": CreateClientPlugin returned null");
            return false;
        }

        const std::string plugin_config = entry.dump();
        try {
            if (!plugin->init(plugin_config)) {
                LOG_ERROR("client plugin " << name << ": init failed");
                destroy(plugin);
                return false;
            }
        } catch (const std::exception& e) {
            LOG_ERROR("client plugin " << name << ": init threw: " << e.what());
            destroy(plugin);
            return false;
        } catch (...) {
            LOG_ERROR("client plugin " << name << ": init threw unknown exception");
            destroy(plugin);
            return false;
        }

        if (plugin_ == nullptr) {
            plugin_ = plugin;
        } else {
            LOG_WARN("client plugin " << name
                                     << " loaded but only the first plugin executes tasks");
        }
        plugins_.push_back(
            {std::move(library),
             std::unique_ptr<common::ClientPlugin, ClientPluginDeleter>(
                 plugin, ClientPluginDeleter{destroy})});
        LOG_INFO("client plugin loaded: " << name << " task_type=" << plugin->taskType());
    }
    return true;
}

void Client::RunPoll() {
    auto last_heartbeat = std::chrono::steady_clock::now();
    while (running_.load(std::memory_order_relaxed)) {
        if (dealer_.waitReadable(poll_timeout_)) {
            try {
                const std::string payload = dealer_.recvMsg();
                common::Envelope env;
                if (env.ParseFromString(payload)) {
                    HandleMessage(env);
                } else {
                    LOG_WARN("client: unparseable message");
                }
            } catch (const std::exception& e) {
                // A zmq failure means the network layer is broken.
                LOG_ERROR("client: fatal zmq error: " << e.what());
                std::terminate();
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_heartbeat >= heartbeat_interval_) {
            last_heartbeat = now;
            SendHeartbeat();
        }

        common::Envelope out;
        while (outgoing_.pop(out, std::chrono::milliseconds(0))) {
            std::string bytes;
            if (out.SerializeToString(&bytes)) {
                dealer_.sendMsg(bytes);
            } else {
                LOG_ERROR("client: failed to serialize outgoing envelope");
            }
        }
    }
}

void Client::RunExecutor() {
    while (running_.load(std::memory_order_relaxed)) {
        common::TaskSpec task;
        if (!tasks_.pop(task, std::chrono::milliseconds(100))) {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            busy_ = true;
            current_task_id_ = task.task_id();
        }

        common::TaskResult result;
        result.set_task_id(task.task_id());
        bool ok = false;
        if (plugin_ == nullptr) {
            LOG_ERROR("client: no plugin to execute task " << task.task_id());
        } else {
            try {
                ok = plugin_->execute(task, result);
            } catch (const std::exception& e) {
                LOG_ERROR("client plugin " << plugin_->taskType() << ": execute threw: " << e.what());
            } catch (...) {
                LOG_ERROR("client plugin " << plugin_->taskType()
                                          << ": execute threw unknown exception");
            }
        }
        if (!ok) {
            result.set_exit_code(-1);
        }

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            busy_ = false;
            current_task_id_.clear();
        }
        SendResponse(result);
    }
}

void Client::HandleMessage(const common::Envelope& env) {
    switch (env.type()) {
        case common::RUN_WAVE:
            if (env.has_run_wave()) {
                HandleRunWave(env.run_wave());
            }
            break;
        case common::STOP:
            LOG_WARN("client: STOP received, cancellation not implemented yet");
            break;
        case common::ACK:
        case common::RESPONSE:
        case common::HEARTBEAT:
            LOG_WARN("client: unexpected message type " << env.type());
            break;
        default:
            LOG_WARN("client: unhandled message type " << env.type());
            break;
    }
}

void Client::HandleRunWave(const common::RunWaveCommand& command) {
    LOG_INFO("client: run " << command.run_id() << " wave " << command.wave_id() << " tasks="
                            << command.tasks_size());
    for (const common::TaskSpec& task : command.tasks()) {
        tasks_.push(task);
    }
}

void Client::SendHeartbeat() {
    common::Envelope env;
    env.set_type(common::HEARTBEAT);
    common::Heartbeat* heartbeat = env.mutable_heartbeat();
    heartbeat->set_worker_id(worker_id_);
    heartbeat->set_timestamp_ms(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        heartbeat->set_status(busy_ ? common::Heartbeat::BUSY : common::Heartbeat::IDLE);
    }
    heartbeat->set_program_version(program_version_);
    heartbeat->set_dataset_version(dataset_version_);
    heartbeat->set_dataset_ready(dataset_ready_);

    std::string bytes;
    if (env.SerializeToString(&bytes)) {
        dealer_.sendMsg(bytes);
    }
}

void Client::SendResponse(const common::TaskResult& result) {
    common::Envelope env;
    env.set_type(common::RESPONSE);
    env.set_request_id(result.task_id());
    common::Response* response = env.mutable_response();
    response->set_request_id(result.task_id());
    response->set_status(common::OK);
    *response->mutable_run_wave()->add_tasks() = result;
    outgoing_.push(std::move(env));
}

}  // namespace client
