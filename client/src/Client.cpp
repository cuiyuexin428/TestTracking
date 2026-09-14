#include "client/Client.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "common/Logger.h"
#include "common/Protocol.h"

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
// clang-format off: WinSock must be included before windows.h.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <psapi.h>
// clang-format on
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#endif

namespace client {

namespace {

std::string DetectLocalIp() {
#ifdef _WIN32
    ULONG size = 0;
    if (GetAdaptersAddresses(
            AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW ||
        size == 0) {
        return std::string();
    }
    std::vector<BYTE> buffer(size);
    PIP_ADAPTER_ADDRESSES addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
    if (GetAdaptersAddresses(
            AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, addresses, &size) != NO_ERROR) {
        return std::string();
    }
    for (PIP_ADAPTER_ADDRESSES adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) {
            continue;
        }
        for (PIP_ADAPTER_UNICAST_ADDRESS unicast = adapter->FirstUnicastAddress; unicast != nullptr;
             unicast = unicast->Next) {
            if (unicast->Address.lpSockaddr->sa_family != AF_INET) {
                continue;
            }
            const auto* sin = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
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

std::string GenerateRegistrationId() {
    static std::atomic<unsigned long long> sequence{0};
    const auto now = std::chrono::steady_clock::now();
    return "register-" + std::to_string(now.time_since_epoch().count()) + "-" +
           std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
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

    ULONGLONG now_100ns = (static_cast<ULONGLONG>(GetTickCount64()) * 10000ULL);
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

Client::Client(common::Config& config, std::shared_ptr<std::atomic<bool>> stop_signal)
    : config_(config),
      dealer_(config.get("scheduler.endpoint", std::string("tcp://127.0.0.1:5555"))),
      stop_requested_(stop_signal ? std::move(stop_signal)
                                  : std::make_shared<std::atomic<bool>>(false)),
      task_slots_(std::max(1, config.get("client.task_threads", 1))),
      worker_id_(config.get("client.worker_id", std::string("worker-1"))),
      ip_(config.get("client.ip", std::string())),
      heartbeat_interval_(std::max(1, config.get("client.heartbeat_interval_ms", 1000))),
      poll_timeout_(std::clamp(config.get("scheduler.poll_timeout_ms", 50), 1, 100)),
      request_backoff_initial_(std::max(1, config.get("client.request_backoff_initial_ms", 200))),
      request_backoff_max_(std::max(1, config.get("client.request_backoff_max_ms", 30000))),
      register_timeout_ms_(config.get("client.register_timeout_ms", 5000)),
      register_max_attempts_(std::max(1, config.get("client.register_max_attempts", 5))),
      request_timeout_ms_(std::max(100, config.get("client.request_timeout_ms", 5000))),
      report_retry_ms_(std::max(50, config.get("client.report_retry_ms", 500))),
      outgoing_limit_(static_cast<size_t>(task_slots_) * 4 + 32) {
    request_backoff_max_ = std::max(request_backoff_initial_, request_backoff_max_);
}

Client::~Client() {
    stop();
    // Join before completed_results_ and plugin instances are destroyed.
    executor_.stop();
}

void Client::start() {
    if (stop_requested_->load()) return;
    if (!LoadPlugins()) throw std::runtime_error("Client: plugin loading failed");
    dealer_.setOption(ZMQ_IDENTITY, worker_id_.data(), worker_id_.size());
    const int receive_timeout = static_cast<int>(poll_timeout_.count());
    const int send_timeout = 0;
    const int high_water = static_cast<int>(outgoing_limit_);
    const int immediate = 1;
    dealer_.setOption(ZMQ_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));
    dealer_.setOption(ZMQ_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    dealer_.setOption(ZMQ_SNDHWM, &high_water, sizeof(high_water));
    dealer_.setOption(ZMQ_RCVHWM, &high_water, sizeof(high_water));
    dealer_.setOption(ZMQ_IMMEDIATE, &immediate, sizeof(immediate));
    dealer_.connect();
    if (ip_.empty()) ip_ = DetectLocalIp();
    LOG_INFO("client local ip=" << ip_);
    PrepareRegistration();
    RunStateMachine();
    executor_.stop();
}

void Client::stop() {
    // No later start assignment can erase this monotonic stop request.
    stop_requested_->store(true, std::memory_order_release);
}

bool Client::LoadPlugins() {
    const auto* entries = config_.find("plugins");
    if (!entries || !entries->is_object()) {
        LOG_ERROR("client requires exactly one enabled plugin");
        return false;
    }
    int enabled = 0;
    for (const auto& entry : entries->items()) {
        if (entry.value().is_object() && entry.value().value("enabled", false)) ++enabled;
    }
    if (enabled != 1) {
        LOG_ERROR("client requires exactly one enabled plugin; configured " << enabled);
        return false;
    }
    for (const auto& entry : entries->items()) {
        if (!entry.value().is_object() || !entry.value().value("enabled", false)) continue;
        const auto dll = entry.value().value("dll", std::string());
        auto library = common::DynamicLibrary::load(dll);
        if (!library) {
            LOG_ERROR("client plugin " << entry.key() << ": cannot load " << dll << ": "
                                       << common::DynamicLibrary::lastError());
            return false;
        }
        const auto create = library->get<common::CreateClientPluginFn>("CreateClientPlugin");
        const auto destroy = library->get<common::DestroyClientPluginFn>("DestroyClientPlugin");
        if (!create || !destroy) {
            LOG_ERROR("client plugin " << entry.key() << ": missing factory exports");
            return false;
        }
        std::unique_ptr<common::WorkerPlugin, ClientPluginDeleter> instance(
            create(), ClientPluginDeleter{destroy});
        if (!instance || !instance->init(entry.value().dump())) return false;
        plugin_ = instance.get();
        plugins_.push_back({std::move(library), std::move(instance)});
        LOG_INFO("client plugin loaded: " << entry.key()
                                          << " plugin_type=" << plugin_->pluginType());
    }
    return plugin_ != nullptr;
}

void Client::PrepareRegistration() {
    RefreshRegistration();
    registration_attempts_ = 0;
    registration_started_ = next_registration_;
}

void Client::RefreshRegistration() {
    std::string payload;
    if (!plugin_->collectRegistrationInfo(&payload)) {
        throw std::runtime_error("Client: collectRegistrationInfo failed");
    }
    registration_.Clear();
    registration_.set_type(common::REGISTER);
    registration_.set_plugin_type(plugin_->pluginType());
    registration_.mutable_register_()->set_payload(payload);
    registration_.mutable_register_()->set_protocol_version(common::kProtocolVersion);
    registration_.mutable_register_()->add_capabilities(common::kAttemptIdsCapability);
    registration_id_ = GenerateRegistrationId();
    registration_.mutable_register_()->set_registration_id(registration_id_);
    next_registration_ = std::chrono::steady_clock::now();
}

void Client::SendRegistration(std::chrono::steady_clock::time_point now) {
    if (now < next_registration_) return;
    const bool initial_attempt_limit =
        !has_registered_ && registration_attempts_ >= register_max_attempts_;
    const bool initial_deadline =
        register_timeout_ms_ > 0 &&
        now - registration_started_ >=
            std::chrono::milliseconds(static_cast<std::int64_t>(register_timeout_ms_) *
                                      register_max_attempts_);
    if (!has_registered_ && (initial_attempt_limit || initial_deadline)) {
        throw std::runtime_error("Client: registration failed after configured attempts");
    }
    ++registration_attempts_;
    const bool sent = SendEnvelope(registration_);
    if (!sent) {
        --registration_attempts_;
        next_registration_ = now + std::chrono::milliseconds(50);
        return;
    }
    LOG_INFO("register attempt " << registration_attempts_
                                 << (sent ? " sent" : " awaiting connection")
                                 << " for plugin_type=" << plugin_->pluginType());
    // Initial finite attempts fail clearly; a previously registered client keeps
    // reconnecting even while paused. All waits remain inside the event loop.
    next_registration_ =
        now + std::chrono::milliseconds(register_timeout_ms_ > 0 ? register_timeout_ms_ : 5000);
}

void Client::HandleRegistration(const common::Envelope& envelope) {
    if (registered_ || !envelope.has_register_result() ||
        envelope.register_result().registration_id() != registration_id_)
        return;
    const auto& reply = envelope.register_result();
    if (reply.protocol_version() != common::kProtocolVersion ||
        std::find(reply.capabilities().begin(), reply.capabilities().end(),
                  common::kAttemptIdsCapability) == reply.capabilities().end()) {
        throw std::runtime_error("Client: incompatible scheduler protocol (version=" +
                                 std::to_string(reply.protocol_version()) +
                                 ", required version=" + std::to_string(common::kProtocolVersion) +
                                 ", required capability=" + common::kAttemptIdsCapability + ")");
    }
    if (!reply.plugin_found() || !reply.accepted() || envelope.session_id().empty()) {
        throw std::runtime_error("Client: registration rejected: " + reply.reason());
    }
    const auto outcome = plugin_->handleValidationReply(reply.payload());
    if (outcome == common::RegisterOutcome::kFailed) {
        throw std::runtime_error("Client: plugin rejected registration reply");
    }
    if (outcome == common::RegisterOutcome::kRetry) {
        // The plugin may refresh its deployment metadata, but the reply still
        // consumed one attempt from the initial registration budget.
        RefreshRegistration();
        return;
    }
    session_id_ = envelope.session_id();
    registered_ = true;
    has_registered_ = true;
    last_server_message_ = std::chrono::steady_clock::now();
    last_heartbeat_ = last_server_message_ - heartbeat_interval_;
    next_request_ = last_server_message_;
    backoff_ = request_backoff_initial_;
    LOG_INFO("registered: plugin_type=" << plugin_->pluginType());
    PollExecutor();
}

void Client::CancelExecution() {
    if (executor_started_) {
        executor_.requestStop();
        draining_executor_ = true;
    }
    active_tasks_.clear();
    pending_reports_.clear();
    completed_results_.clear();
    outgoing_.clear();
    outstanding_requests_ = 0;
}

void Client::BeginRegistration() {
    if (!registered_) return;
    LOG_WARN("client: session lost; re-registering");
    registered_ = false;
    session_id_.clear();
    CancelExecution();
    // Reuse deployment metadata for reconnects; validation may request refresh.
    registration_id_ = GenerateRegistrationId();
    registration_.mutable_register_()->set_registration_id(registration_id_);
    registration_attempts_ = 0;
    next_registration_ = std::chrono::steady_clock::now();
}

void Client::PollExecutor() {
    if (draining_executor_) {
        if (!executor_.isStopped()) return;
        executor_.stop();
        executor_started_ = false;
        draining_executor_ = false;
        completed_results_.clear();
    }
    if (!executor_started_ && registered_ && !silent_) {
        executor_.start(
            plugin_, [this](const common::TaskResult& result) { completed_results_.push(result); },
            task_slots_);
        executor_started_ = true;
    }
}

void Client::RunStateMachine() {
    last_heartbeat_ = std::chrono::steady_clock::now();
    last_server_message_ = last_heartbeat_;
    next_request_ = last_heartbeat_;
    backoff_ = request_backoff_initial_;
    while (!stop_requested_->load(std::memory_order_acquire)) {
        if (dealer_.waitReadable(poll_timeout_)) {
            for (int received = 0; received < 32; ++received) {
                common::Envelope envelope;
                if (envelope.ParseFromString(dealer_.recvMsg())) HandleEnvelope(envelope);
                if (!dealer_.waitReadable(std::chrono::milliseconds(0))) break;
            }
        }
        PollExecutor();
        const auto now = std::chrono::steady_clock::now();
        if (!registered_) {
            SendRegistration(now);
            continue;
        }
        const auto session_timeout =
            std::max(std::chrono::milliseconds(request_timeout_ms_), heartbeat_interval_ * 3);
        if (now - last_server_message_ > session_timeout) {
            BeginRegistration();
            continue;
        }
        if (now - last_heartbeat_ >= heartbeat_interval_) {
            last_heartbeat_ = now;
            SendHeartbeat();
        }
        FlushOutgoing();
        CollectCompletedResults();
        RetryReports(now);
        if (outstanding_requests_ &&
            now - request_sent_at_ > std::chrono::milliseconds(request_timeout_ms_)) {
            outstanding_requests_ = 0;
        }
        const int free_slots = task_slots_ - static_cast<int>(active_tasks_.size());
        if (!silent_ && !draining_executor_ && executor_started_ && outgoing_.empty() &&
            free_slots > 0 && outstanding_requests_ == 0 && now >= next_request_) {
            if (SendRequestTask(free_slots)) {
                outstanding_requests_ = 1;
                request_sent_at_ = now;
            }
        }
    }
    executor_.requestStop();
}

void Client::HandleEnvelope(const common::Envelope& env) {
    if (env.type() == common::REGISTER_RESULT) {
        HandleRegistration(env);
        return;
    }
    if (!registered_ || env.session_id() != session_id_) return;
    last_server_message_ = std::chrono::steady_clock::now();
    switch (env.type()) {
        case common::HEARTBEAT_ACK:
            if (env.has_heartbeat_ack() && !env.heartbeat_ack().registered()) BeginRegistration();
            break;
        case common::TASK:
            if (env.has_task()) HandleTask(env.task(), env.plugin_type());
            break;
        case common::NO_TASK:
            if (outstanding_requests_) {
                outstanding_requests_ = 0;
                backoff_ = std::min(backoff_ * 2, request_backoff_max_);
                next_request_ = last_server_message_ + backoff_;
            }
            break;
        case common::STOP:
            if (env.has_stop()) HandleStop(env.stop().run_id(), env.stop().reason());
            break;
        case common::WAKE:
            silent_ = false;
            backoff_ = request_backoff_initial_;
            next_request_ = last_server_message_;
            LOG_INFO("client: wakeup received, resuming");
            break;
        case common::TASK_REPORT_ACK:
            if (env.has_task_report_ack()) {
                const auto& ack = env.task_report_ack();
                const TaskKey key{ack.task_id(), ack.attempt_id()};
                const auto report = pending_reports_.find(key);
                if (report == pending_reports_.end()) break;
                if (ack.accepted() || !ack.retryable()) {
                    if (!ack.accepted()) LOG_WARN("client: report rejected: " << ack.reason());
                    pending_reports_.erase(report);
                    active_tasks_.erase(key);
                    backoff_ = request_backoff_initial_;
                    next_request_ = last_server_message_;
                } else {
                    report->second.retry_at =
                        last_server_message_ + std::chrono::milliseconds(report_retry_ms_);
                }
            }
            break;
        default:
            LOG_WARN("client: unexpected message type " << env.type());
            break;
    }
}

void Client::HandleStop(const std::string& run_id, const std::string& reason) {
    LOG_WARN("client: STOP received run_id=" << run_id << " reason=" << reason);
    silent_ = true;
    CancelExecution();
    backoff_ = request_backoff_initial_;
}

void Client::HandleTask(const common::TaskCommand& command, const std::string& plugin_type) {
    LOG_INFO("client: task batch run=" << command.run_id() << " tasks=" << command.tasks_size()
                                       << " plugin_type=" << plugin_type);
    outstanding_requests_ = 0;
    backoff_ = request_backoff_initial_;
    for (auto task : command.tasks()) {
        if (task.run_id().empty()) task.set_run_id(command.run_id());
        const TaskKey key{task.task_id(), task.attempt_id()};
        const bool duplicate = active_tasks_.count(key) != 0;
        const bool acceptable =
            plugin_type == plugin_->pluginType() && !silent_ && !draining_executor_ &&
            executor_started_ && !task.attempt_id().empty() && task.run_id() == command.run_id() &&
            (duplicate || active_tasks_.size() < static_cast<size_t>(task_slots_));
        common::Envelope response;
        response.set_type(acceptable ? common::TASK_ACK : common::TASK_REJECT);
        response.set_plugin_type(plugin_type);
        response.set_session_id(session_id_);
        if (acceptable) {
            response.mutable_task_ack()->set_task_id(task.task_id());
            response.mutable_task_ack()->set_attempt_id(task.attempt_id());
        } else {
            response.mutable_task_reject()->set_task_id(task.task_id());
            response.mutable_task_reject()->set_attempt_id(task.attempt_id());
            response.mutable_task_reject()->set_reason(
                "paused, full, stale attempt, or plugin mismatch");
        }
        // If the control queue is full, leave the task unacknowledged so the
        // scheduler can recover it. Never start work whose receipt cannot queue.
        if (!QueueControl(std::move(response)) || !acceptable || duplicate) continue;
        active_tasks_.emplace(key, task);
        executor_.push(task);
    }
}

bool Client::SendEnvelope(const common::Envelope& envelope) {
    std::string bytes;
    if (!envelope.SerializeToString(&bytes))
        throw std::runtime_error("Client: message serialization failed");
    return dealer_.trySendMsg(bytes);
}

bool Client::QueueControl(common::Envelope envelope) {
    if (outgoing_.size() >= outgoing_limit_) return false;
    outgoing_.push_back(std::move(envelope));
    return true;
}

void Client::FlushOutgoing() {
    for (int sent = 0; sent < 32 && !outgoing_.empty(); ++sent) {
        if (!SendEnvelope(outgoing_.front())) break;
        outgoing_.pop_front();
    }
}

void Client::CollectCompletedResults() {
    common::TaskResult result;
    while (completed_results_.pop(result, std::chrono::milliseconds(0))) {
        const TaskKey key{result.task_id(), result.attempt_id()};
        const auto active = active_tasks_.find(key);
        if (active == active_tasks_.end() || active->second.run_id() != result.run_id()) continue;
        pending_reports_.emplace(key, PendingReport{result, {}});
    }
}

void Client::RetryReports(std::chrono::steady_clock::time_point now) {
    int count = 0;
    for (auto& entry : pending_reports_) {
        auto& pending = entry.second;
        if (pending.retry_at > now) continue;
        common::Envelope envelope;
        envelope.set_type(common::TASK_REPORT);
        envelope.set_plugin_type(plugin_->pluginType());
        envelope.set_session_id(session_id_);
        *envelope.mutable_task_report()->mutable_result() = pending.result;
        if (!SendEnvelope(envelope)) break;
        pending.retry_at = now + std::chrono::milliseconds(report_retry_ms_);
        if (++count >= 32) break;
    }
}

bool Client::SendRequestTask(int count) {
    common::Envelope envelope;
    envelope.set_type(common::REQUEST_TASK);
    envelope.set_plugin_type(plugin_->pluginType());
    envelope.set_session_id(session_id_);
    envelope.mutable_request_task()->set_count(count);
    return SendEnvelope(envelope);
}

void Client::SendHeartbeat() {
    common::Envelope envelope;
    envelope.set_type(common::HEARTBEAT);
    envelope.set_plugin_type(plugin_->pluginType());
    envelope.set_session_id(session_id_);
    auto* heartbeat = envelope.mutable_heartbeat();
    heartbeat->set_ip(ip_);
    heartbeat->set_plugin_info(plugin_->heartbeatInfo());
    heartbeat->set_status(active_tasks_.empty() ? common::Heartbeat::IDLE
                                                : common::Heartbeat::BUSY);
    auto* resource = heartbeat->mutable_resource();
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS memory{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
        resource->set_memory_bytes(static_cast<std::int64_t>(memory.WorkingSetSize));
    ULARGE_INTEGER free_bytes{};
    if (GetDiskFreeSpaceExA(nullptr, &free_bytes, nullptr, nullptr))
        resource->set_disk_free_bytes(static_cast<std::int64_t>(free_bytes.QuadPart));
    resource->set_cpu_percent(SampleProcessCpuPercent());
#else
    (void)resource;
#endif
    SendEnvelope(envelope);  // Transient telemetry is naturally refreshed next interval.
}

}  // namespace client
