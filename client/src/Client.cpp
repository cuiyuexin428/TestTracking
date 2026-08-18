#include "client/Client.h"

#include "common/Logger.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <sdkddkver.h>
#ifndef NTDDI_VERSION
#define NTDDI_VERSION NTDDI_WIN10
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
#include <iphlpapi.h>
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#endif

namespace client {

namespace {

std::string DetectLocalIp() {
#ifdef _WIN32
    ULONG size = 0;
    if (GetAdaptersAddresses(AF_INET,
                             GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                 GAA_FLAG_SKIP_DNS_SERVER,
                             nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW ||
        size == 0) {
        return std::string();
    }
    std::vector<BYTE> buffer(size);
    PIP_ADAPTER_ADDRESSES addresses =
        reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
    if (GetAdaptersAddresses(AF_INET,
                             GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                 GAA_FLAG_SKIP_DNS_SERVER,
                             nullptr, addresses, &size) != NO_ERROR) {
        return std::string();
    }
    for (PIP_ADAPTER_ADDRESSES adapter = addresses; adapter != nullptr;
         adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) {
            continue;
        }
        for (PIP_ADAPTER_UNICAST_ADDRESS unicast = adapter->FirstUnicastAddress;
             unicast != nullptr; unicast = unicast->Next) {
            if (unicast->Address.lpSockaddr->sa_family != AF_INET) {
                continue;
            }
            const auto* sin =
                reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
            const std::string result(ip);
            if (result.rfind("127.", 0) != 0) {
                return result;
            }
        }
    }
#else
    (void)0;
#endif
    return std::string();
}
#ifdef _WIN32
int SampleProcessCpuPercent() {
    // Process CPU usage since the previous heartbeat, clamped to [0, 100].
    static FILETIME last_kernel{};
    static FILETIME last_user{};
    static ULONGLONG last_wall_100ns = 0;
    static bool has_last = false;

    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        return 0;
    }

    ULARGE_INTEGER k;
    ULARGE_INTEGER u;
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;

    ULONGLONG now_100ns =
        (static_cast<ULONGLONG>(GetTickCount64()) * 10000ULL);
    if (!has_last) {
        last_kernel = kernel;
        last_user = user;
        last_wall_100ns = now_100ns;
        has_last = true;
        return 0;
    }

    ULARGE_INTEGER lk;
    ULARGE_INTEGER lu;
    lk.LowPart = last_kernel.dwLowDateTime;
    lk.HighPart = last_kernel.dwHighDateTime;
    lu.LowPart = last_user.dwLowDateTime;
    lu.HighPart = last_user.dwHighDateTime;

    const ULONGLONG cpu_delta = (k.QuadPart - lk.QuadPart) + (u.QuadPart - lu.QuadPart);
    const ULONGLONG wall_delta = now_100ns - last_wall_100ns;
    last_kernel = kernel;
    last_user = user;
    last_wall_100ns = now_100ns;

    if (wall_delta == 0) {
        return 0;
    }
    const ULONGLONG percent = (cpu_delta * 100ULL) / wall_delta;
    return percent > 100ULL ? 100 : static_cast<int>(percent);
}
#endif

}  // namespace

Client::Client(common::Config& config)
    : config_(config),
      dealer_(config.get("scheduler.endpoint", std::string("tcp://127.0.0.1:5555"))),
      task_slots_(std::max(1, config.get("client.task_threads", 1))),
      worker_id_(config.get("client.worker_id", std::string("worker-1"))),
      ip_(config.get("client.ip", std::string())),
      heartbeat_interval_(config.get("client.heartbeat_interval_ms", 1000)),
      poll_timeout_(config.get("scheduler.poll_timeout_ms", 50)),
      request_backoff_initial_(config.get("client.request_backoff_initial_ms", 200)),
      request_backoff_max_(config.get("client.request_backoff_max_ms", 30000)),
      register_timeout_ms_(config.get("client.register_timeout_ms", 5000)),
      register_max_attempts_(config.get("client.register_max_attempts", 5)) {
}

Client::~Client() {
    stop();
}

void Client::start() {
    if (!LoadPlugins()) {
        throw std::runtime_error("Client: plugin loading failed");
    }
    if (plugin_ == nullptr) {
        throw std::runtime_error("Client: no enabled plugin");
    }

    dealer_.setOption(ZMQ_IDENTITY, worker_id_.data(), worker_id_.size());
    const int recv_timeout_ms = static_cast<int>(poll_timeout_.count());
    dealer_.setOption(ZMQ_RCVTIMEO, &recv_timeout_ms, sizeof(recv_timeout_ms));
    dealer_.connect();
    if (ip_.empty()) {
        ip_ = DetectLocalIp();
    }
    LOG_INFO("client local ip=" << ip_);

    // INIT: no threads yet, synchronously complete registration validation.
    if (!RegisterWithServer()) {
        throw std::runtime_error("Client: registration failed");
    }
    if (stop_requested_.load()) {
        return;
    }

    // INIT passed: start the executor worker pool.
    executor_.start(plugin_,
                    [this](const common::TaskResult& result) { OnTaskCompleted(result); },
                    task_slots_);

    running_.store(true);
    RunStateMachine();
}

void Client::stop() {
    stop_requested_.store(true);
    if (!running_.exchange(false)) {
        return;
    }
    executor_.stop();
    {
        std::lock_guard<std::mutex> lock(counters_mutex_);
        in_flight_ = 0;
        outstanding_requests_ = 0;
    }
}

bool Client::LoadPlugins() {
    const nlohmann::json* plugins_node = config_.find("plugins");
    if (plugins_node == nullptr || !plugins_node->is_object()) {
        LOG_WARN("no plugins configured");
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

        common::WorkerPlugin* plugin = nullptr;
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
                                     << " loaded but only the first plugin registers/executes");
        }
        plugins_.push_back(
            {std::move(library),
             std::unique_ptr<common::WorkerPlugin, ClientPluginDeleter>(
                 plugin, ClientPluginDeleter{destroy})});
        LOG_INFO("client plugin loaded: " << name << " plugin_type=" << plugin->pluginType());
    }
    return true;
}

bool Client::RegisterWithServer() {
    for (int attempt = 1; attempt <= register_max_attempts_; ++attempt) {
        if (!SendRegisterOnce()) {
            return false;
        }
        LOG_INFO("register attempt " << attempt << " sent for plugin_type="
                                     << plugin_->pluginType());

        common::Envelope reply;
        if (!WaitForRegisterResult(reply)) {
            LOG_WARN("register attempt " << attempt << " timed out, retrying");
            continue;
        }
        if (!reply.has_register_result()) {
            LOG_WARN("register attempt " << attempt << ": unexpected reply, retrying");
            continue;
        }

        const common::RegisterResult& result = reply.register_result();
        if (!result.plugin_found()) {
            LOG_ERROR("client: registration rejected: server has no plugin for "
                      << plugin_->pluginType());
            return false;
        }
        if (!result.accepted()) {
            LOG_ERROR("client: registration rejected: " << result.reason());
            return false;
        }
        try {
            switch (plugin_->handleValidationReply(result.payload())) {
                case common::RegisterOutcome::kReady:
                    LOG_INFO("registered: plugin_type=" << plugin_->pluginType());
                    return true;
                case common::RegisterOutcome::kRetry:
                    LOG_WARN("registration reply handled, re-registering");
                    continue;
                case common::RegisterOutcome::kFailed:
                    LOG_ERROR("registration failed: plugin rejected");
                    return false;
            }
        } catch (const std::exception& e) {
            LOG_ERROR("client plugin " << plugin_->pluginType()
                                       << ": handleValidationReply threw: " << e.what());
            return false;
        }
    }
    LOG_ERROR("registration failed after " << register_max_attempts_ << " attempts");
    return false;
}

bool Client::SendRegisterOnce() {
    std::string payload;
    try {
        if (!plugin_->collectRegistrationInfo(&payload)) {
            LOG_ERROR("client plugin " << plugin_->pluginType()
                                       << ": collectRegistrationInfo failed");
            return false;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("client plugin " << plugin_->pluginType()
                                   << ": collectRegistrationInfo threw: " << e.what());
        return false;
    }

    common::Envelope env;
    env.set_type(common::REGISTER);
    env.set_plugin_type(plugin_->pluginType());
    common::RegisterCommand* reg = env.mutable_register_();
    reg->set_payload(payload);

    std::string bytes;
    if (!env.SerializeToString(&bytes)) {
        LOG_ERROR("client: failed to serialize REGISTER");
        return false;
    }
    try {
        dealer_.sendMsg(bytes);
    } catch (const std::exception& e) {
        LOG_ERROR("client: send REGISTER failed: " << e.what());
        return false;
    }
    return true;
}

bool Client::WaitForRegisterResult(common::Envelope& out) {
    const auto start = std::chrono::steady_clock::now();
    const bool infinite = register_timeout_ms_ <= 0;
    const auto deadline = start + std::chrono::milliseconds(register_timeout_ms_);

    while (true) {
        if (stop_requested_.load(std::memory_order_relaxed)) {
            LOG_WARN("client: stop requested during registration");
            return false;
        }
        if (dealer_.waitReadable(poll_timeout_)) {
            try {
                const std::string payload = dealer_.recvMsg();
                common::Envelope env;
                if (!env.ParseFromString(payload)) {
                    LOG_WARN("client: unparseable message during registration");
                    continue;
                }
                if (env.type() == common::REGISTER_RESULT) {
                    out = std::move(env);
                    return true;
                }
                LOG_WARN("client: unexpected message type " << env.type()
                                                            << " during registration");
                continue;
            } catch (const std::exception& e) {
                LOG_ERROR("client: recv failed during registration: " << e.what());
                return false;
            }
        }
        if (!infinite && std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
    }
}

void Client::RunStateMachine() {
    last_heartbeat_ = std::chrono::steady_clock::now();
    next_request_ = last_heartbeat_;
    backoff_ = request_backoff_initial_;

    while (running_.load(std::memory_order_relaxed)) {
        if (dealer_.waitReadable(poll_timeout_)) {
            try {
                const std::string payload = dealer_.recvMsg();
                common::Envelope env;
                if (env.ParseFromString(payload)) {
                    HandleEnvelope(env);
                } else {
                    LOG_WARN("client: unparseable message");
                }
            } catch (const std::exception& e) {
                LOG_ERROR("client: fatal zmq error: " << e.what());
                std::terminate();
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_heartbeat_ >= heartbeat_interval_) {
            last_heartbeat_ = now;
            SendHeartbeat();
        }

        // Report completed tasks; workers queue results as they finish.
        common::TaskResult result;
        while (completed_results_.pop(result, std::chrono::milliseconds(0))) {
            SendTaskReport(result);
            backoff_ = request_backoff_initial_;
        }

        // Ask for all free slots in one request; the server grants up to
        // count tasks in a single reply. At most one request is outstanding.
        if (!silent_.load(std::memory_order_relaxed)) {
            int free_slots = 0;
            {
                std::lock_guard<std::mutex> lock(counters_mutex_);
                free_slots = task_slots_ - in_flight_ - outstanding_requests_;
            }
            if (free_slots > 0 && outstanding_requests_ == 0 && now >= next_request_) {
                if (SendRequestTask(free_slots)) {
                    std::lock_guard<std::mutex> lock(counters_mutex_);
                    outstanding_requests_ = 1;
                }
            }
        }

        try {
            common::Envelope out;
            while (outgoing_.pop(out, std::chrono::milliseconds(0))) {
                std::string bytes;
                if (out.SerializeToString(&bytes)) {
                    dealer_.sendMsg(bytes);
                } else {
                    LOG_ERROR("client: failed to serialize outgoing envelope");
                }
            }
        } catch (const std::exception& e) {
            LOG_ERROR("client: fatal zmq error: " << e.what());
            std::terminate();
        } catch (...) {
            LOG_ERROR("client: fatal zmq unknown error");
            std::terminate();
        }
    }
}

void Client::HandleEnvelope(const common::Envelope& env) {
    switch (env.type()) {
        case common::TASK:
            if (env.has_task()) {
                HandleTask(env.task(), env.plugin_type());
                backoff_ = request_backoff_initial_;
            }
            break;
        case common::NO_TASK: {
            std::lock_guard<std::mutex> lock(counters_mutex_);
            if (outstanding_requests_ > 0) {
                --outstanding_requests_;
                if (outstanding_requests_ == 0) {
                    backoff_ = std::min(backoff_ * 2, request_backoff_max_);
                    next_request_ = std::chrono::steady_clock::now() + backoff_;
                    LOG_INFO("client: no task, next request in " << backoff_.count() << " ms");
                }
            }
            break;
        }
        case common::STOP:
            if (env.has_stop()) {
                HandleStop(env.stop().run_id(), env.stop().reason());
            }
            break;
        case common::WAKE: {
            const bool was_silent = silent_.exchange(false);
            backoff_ = request_backoff_initial_;
            {
                std::lock_guard<std::mutex> lock(counters_mutex_);
                next_request_ = std::chrono::steady_clock::now();
            }
            LOG_INFO("client: wakeup received"
                     << (was_silent ? ", resuming" : " (no-op, already active)"));
            break;
        }
        default:
            HandleMessage(env);
            break;
    }
}

void Client::HandleStop(const std::string& run_id, const std::string& reason) {
    LOG_WARN("client: STOP received run_id=" << run_id << " reason=" << reason
                                             << ", stopping executor and entering silent state");
    // Drops queued tasks; the server reclaims them from its in_flight table.
    executor_.stop();
    // Drop results of the stopped run so they are not reported later.
    completed_results_.clear();
    // Restart a fresh executor so tasks of a new run can be executed.
    executor_.start(plugin_,
                    [this](const common::TaskResult& result) { OnTaskCompleted(result); },
                    task_slots_);
    backoff_ = request_backoff_initial_;
    {
        std::lock_guard<std::mutex> lock(counters_mutex_);
        in_flight_ = 0;
        outstanding_requests_ = 0;
        next_request_ = std::chrono::steady_clock::now();
    }
    silent_.store(true);
}

void Client::HandleMessage(const common::Envelope& env) {
    switch (env.type()) {
        case common::REGISTER_RESULT:
        case common::HEARTBEAT:
        case common::REQUEST_TASK:
        case common::TASK_REPORT:
        case common::REGISTER:
        case common::TASK_ACK:
        case common::TASK_REJECT:
            LOG_WARN("client: unexpected message type " << env.type());
            break;
        default:
            LOG_WARN("client: unhandled message type " << env.type());
            break;
    }
}

void Client::OnTaskCompleted(const common::TaskResult& result) {
    completed_results_.push(result);
    std::lock_guard<std::mutex> lock(counters_mutex_);
    --in_flight_;
}

void Client::HandleTask(const common::TaskCommand& command, const std::string& plugin_type) {
    LOG_INFO("client: task batch run=" << command.run_id()
                                       << " tasks=" << command.tasks_size()
                                       << " plugin_type=" << plugin_type);
    // Defensive: after a successful registration the client and server agree
    // on plugin_type, so this path should be unreachable.
    if (plugin_ != nullptr && plugin_type != plugin_->pluginType()) {
        LOG_WARN("client: plugin_type mismatch, rejecting " << command.tasks_size()
                                                            << " tasks");
        for (const common::Task& task : command.tasks()) {
            common::Envelope reject;
            reject.set_type(common::TASK_REJECT);
            reject.set_plugin_type(plugin_type);
            reject.mutable_task_reject()->set_task_id(task.task_id());
            reject.mutable_task_reject()->set_reason("plugin_type mismatch");
            outgoing_.push(std::move(reject));
        }
        std::lock_guard<std::mutex> lock(counters_mutex_);
        if (outstanding_requests_ > 0) {
            --outstanding_requests_;
        }
        return;
    }

    // Task receipt: confirm delivery before execution.
    for (const common::Task& task : command.tasks()) {
        common::Envelope ack;
        ack.set_type(common::TASK_ACK);
        ack.set_plugin_type(plugin_type);
        ack.mutable_task_ack()->set_task_id(task.task_id());
        outgoing_.push(std::move(ack));
        executor_.push(task);
    }
    std::lock_guard<std::mutex> lock(counters_mutex_);
    if (outstanding_requests_ > 0) {
        --outstanding_requests_;
    }
    in_flight_ += command.tasks_size();
}

void Client::SendHeartbeat() {
    common::Envelope env;
    env.set_type(common::HEARTBEAT);
    env.set_plugin_type(plugin_ != nullptr ? plugin_->pluginType() : std::string());
    common::Heartbeat* heartbeat = env.mutable_heartbeat();
    heartbeat->set_ip(ip_);
    if (plugin_ != nullptr) {
        heartbeat->set_plugin_info(plugin_->heartbeatInfo());
    }
    {
        std::lock_guard<std::mutex> lock(counters_mutex_);
        heartbeat->set_status(in_flight_ > 0 ? common::Heartbeat::BUSY
                                             : common::Heartbeat::IDLE);
    }
    common::ResourceUsage* resource = heartbeat->mutable_resource();
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        resource->set_memory_bytes(static_cast<std::int64_t>(pmc.WorkingSetSize));
    }
    ULARGE_INTEGER free_bytes{};
    if (GetDiskFreeSpaceExA(nullptr, &free_bytes, nullptr, nullptr)) {
        resource->set_disk_free_bytes(static_cast<std::int64_t>(free_bytes.QuadPart));
    }
    resource->set_cpu_percent(SampleProcessCpuPercent());
#else
    (void)resource;
#endif

    std::string bytes;
    if (!env.SerializeToString(&bytes)) {
        LOG_ERROR("client: failed to serialize HEARTBEAT");
        return;
    }
    try {
        dealer_.sendMsg(bytes);
    } catch (const std::exception& e) {
        LOG_ERROR("client: fatal zmq error: " << e.what());
        std::terminate();
    } catch (...) {
        LOG_ERROR("client: fatal zmq unknown error");
        std::terminate();
    }
}

bool Client::SendRequestTask(int count) {
    if (plugin_ == nullptr || count <= 0) {
        return false;
    }
    common::Envelope env;
    env.set_type(common::REQUEST_TASK);
    env.set_plugin_type(plugin_->pluginType());
    common::RequestTaskCommand* request = env.mutable_request_task();
    request->set_count(count);

    std::string bytes;
    if (!env.SerializeToString(&bytes)) {
        LOG_ERROR("client: failed to serialize REQUEST_TASK");
        return false;
    }
    try {
        dealer_.sendMsg(bytes);
    } catch (const std::exception& e) {
        LOG_ERROR("client: fatal zmq error: " << e.what());
        std::terminate();
    } catch (...) {
        LOG_ERROR("client: fatal zmq unknown error");
        std::terminate();
    }
    return true;
}

void Client::SendTaskReport(const common::TaskResult& result) {
    common::Envelope env;
    env.set_type(common::TASK_REPORT);
    env.set_plugin_type(plugin_ != nullptr ? plugin_->pluginType() : std::string());
    common::TaskReport* report = env.mutable_task_report();
    *report->mutable_result() = result;
    outgoing_.push(std::move(env));
}

}  // namespace client
