#include <algorithm>

#include "common/Logger.h"
#include "common/Protocol.h"
#include "scheduler/Scheduler.h"

namespace scheduler {
namespace {
std::string GenerateId(const char* prefix) {
    static std::atomic<unsigned long long> sequence{0};
    return std::string(prefix) +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
           std::to_string(sequence.fetch_add(1));
}
}  // namespace

bool Scheduler::IsCurrentSession(const IncomingMessage& msg) const {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    const auto it = clients_.find(msg.identity);
    return it != clients_.end() && it->second.info.registered &&
           it->second.info.plugin_type == msg.envelope.plugin_type() &&
           !msg.envelope.session_id().empty() && it->second.session_id == msg.envelope.session_id();
}

void Scheduler::HandleHeartbeat(const IncomingMessage& msg) {
    bool current = false;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        const auto it = clients_.find(msg.identity);
        if (it != clients_.end() && it->second.session_id == msg.envelope.session_id() &&
            it->second.info.plugin_type == msg.envelope.plugin_type()) {
            current = true;
            it->second.info.heartbeat = msg.envelope.heartbeat();
            it->second.last_seen = msg.received_at;
        }
    }
    common::Envelope ack;
    ack.set_type(common::HEARTBEAT_ACK);
    ack.set_plugin_type(msg.envelope.plugin_type());
    ack.set_session_id(msg.envelope.session_id());
    ack.mutable_heartbeat_ack()->set_session_id(msg.envelope.session_id());
    ack.mutable_heartbeat_ack()->set_registered(current);
    server_.sendTo(msg.identity, ack);
}

void Scheduler::HandleMessage(const IncomingMessage& msg) {
    switch (msg.envelope.type()) {
        case common::REGISTER:
            if (msg.envelope.has_register_()) HandleRegister(msg);
            break;
        case common::REQUEST_TASK:
            if (msg.envelope.has_request_task()) HandleRequestTask(msg);
            break;
        case common::TASK_REPORT:
            if (msg.envelope.has_task_report()) HandleTaskReport(msg);
            break;
        case common::TASK_ACK:
            if (msg.envelope.has_task_ack()) HandleTaskAck(msg);
            break;
        case common::TASK_REJECT:
            if (msg.envelope.has_task_reject()) HandleTaskReject(msg);
            break;
        default:
            break;
    }
}

void Scheduler::SendRegisterResult(const IncomingMessage& msg, bool found,
                                   const common::ValidationResult& validation,
                                   const std::string& session) {
    common::Envelope env;
    env.set_type(common::REGISTER_RESULT);
    env.set_plugin_type(msg.envelope.plugin_type());
    env.set_session_id(session);
    auto* result = env.mutable_register_result();
    result->set_plugin_found(found);
    result->set_accepted(validation.accepted);
    result->set_reason(validation.reason);
    result->set_payload(validation.reply_payload);
    result->set_registration_id(msg.envelope.register_().registration_id());
    result->set_protocol_version(common::kProtocolVersion);
    result->add_capabilities(common::kAttemptIdsCapability);
    server_.sendTo(msg.identity, env);
}

void Scheduler::HandleRegister(const IncomingMessage& msg) {
    const auto type = msg.envelope.plugin_type();
    const auto plugin = plugins_by_type_.find(type);
    if (plugin == plugins_by_type_.end()) {
        SendRegisterResult(msg, false, {false, {}, "no plugin for plugin_type " + type}, {});
        return;
    }
    const auto& request = msg.envelope.register_();
    const bool protocol_ok = request.protocol_version() == common::kProtocolVersion &&
        std::find(request.capabilities().begin(), request.capabilities().end(),
                  common::kAttemptIdsCapability) != request.capabilities().end();
    if (!protocol_ok) {
        const std::string reason = "incompatible client protocol: version=" +
            std::to_string(request.protocol_version()) +
            ", required version=" + std::to_string(common::kProtocolVersion) +
            ", required capability=" + common::kAttemptIdsCapability;
        LOG_WARN("register rejected for client " << msg.identity << ": " << reason);
        SendRegisterResult(msg, true, {false, {}, reason}, {});
        return;
    }
    common::ClientInfo client;
    client.identity = msg.identity;
    client.plugin_type = type;
    client.peer_address = msg.peer_address;
    std::string previous_session;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        const auto old = clients_.find(msg.identity);
        if (old != clients_.end()) {
            // Repeated registration packets must not create replacement sessions.
            if (old->second.info.plugin_type == type &&
                old->second.registration_id == msg.envelope.register_().registration_id()) {
                SendRegisterResult(msg, true, old->second.validation, old->second.session_id);
                return;
            }
            previous_session = old->second.session_id;
            client.heartbeat = old->second.info.heartbeat;
        }
    }
    common::ValidationResult validation;
    if (!plugin->second->validateClient(client, msg.envelope.register_().payload(), validation))
        validation.accepted = false;
    if (!validation.accepted) {
        SendRegisterResult(msg, true, validation, {});
        return;
    }
    if (!previous_session.empty()) ReclaimClientTasks(msg.identity, previous_session);
    const auto session = GenerateId("session-");
    client.registered = true;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_[msg.identity] = {client, Clock::now(), session,
                                  msg.envelope.register_().registration_id(), validation};
    }
    SendRegisterResult(msg, true, validation, session);
    LOG_INFO("register: client " << msg.identity << " plugin_type=" << type
                                 << " accepted session=" << session);
}

void Scheduler::SendNoTask(const IncomingMessage& msg) {
    common::Envelope env;
    env.set_type(common::NO_TASK);
    env.set_plugin_type(msg.envelope.plugin_type());
    env.set_session_id(msg.envelope.session_id());
    env.mutable_no_task();
    server_.sendTo(msg.identity, env);
}

void Scheduler::HandleRequestTask(const IncomingMessage& msg) {
    if (!IsCurrentSession(msg)) return;
    auto* plugin = plugins_by_type_.at(msg.envelope.plugin_type());
    const auto active = active_runs_.find(plugin);
    if (active == active_runs_.end() || active->second.stopping) {
        SendNoTask(msg);
        return;
    }
    common::ClientInfo client;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        client = clients_.at(msg.identity).info;
    }
    // Each request asks for free slots, never an unbounded allocation.
    const int count = std::clamp(msg.envelope.request_task().count(), 0, 256);
    common::Envelope env;
    env.set_type(common::TASK);
    env.set_plugin_type(msg.envelope.plugin_type());
    env.set_session_id(msg.envelope.session_id());
    env.mutable_task()->set_run_id(active->second.run_id);
    for (int i = 0; i < count; ++i) {
        common::Task task;
        if (!plugin->requestTask(client, msg.envelope.request_task().payload(), task)) break;
        if (in_flight_.count(task.task_id())) {
            // The plugin handed out a task the framework already tracks: adding it
            // again would double-dispatch it. Stop collecting this batch and send
            // the attempts gathered so far; the tracked attempt keeps its own
            // lifecycle, so nothing is abandoned.
            LOG_ERROR("duplicate plugin task id " << task.task_id()
                                                  << "; sending collected batch only");
            break;
        }
        task.set_run_id(active->second.run_id);
        task.set_attempt_id(GenerateId("attempt-"));
        if (storage_.enabled() && !storage_.SaveTask(task.task_id(), task.run_id(), msg.identity,
                                                     task.payload(), task.attempt_id())) {
            plugin->requeue({task.task_id()});
            break;
        }
        InFlight attempt;
        attempt.identity = msg.identity;
        attempt.session_id = msg.envelope.session_id();
        attempt.plugin_type = msg.envelope.plugin_type();
        attempt.run_id = task.run_id();
        attempt.attempt_id = task.attempt_id();
        attempt.sent_at = Clock::now();
        in_flight_[task.task_id()] = std::move(attempt);
        *env.mutable_task()->add_tasks() = task;
        LOG_INFO("task " << task.task_id() << " -> client " << msg.identity);
    }
    in_flight_count_.store(in_flight_.size());
    if (env.task().tasks().empty())
        SendNoTask(msg);
    else
        server_.sendTo(msg.identity, env);
}

bool Scheduler::Matches(const InFlight& task, const IncomingMessage& msg,
                        const std::string& attempt) const {
    return task.identity == msg.identity && task.session_id == msg.envelope.session_id() &&
           task.plugin_type == msg.envelope.plugin_type() && task.attempt_id == attempt;
}

void Scheduler::SendTaskReportAck(const std::string& identity, const std::string& type,
                                  const std::string& session, const common::TaskResult& result,
                                  bool accepted, bool retryable, const std::string& reason) {
    common::Envelope env;
    env.set_type(common::TASK_REPORT_ACK);
    env.set_plugin_type(type);
    env.set_session_id(session);
    auto* ack = env.mutable_task_report_ack();
    ack->set_task_id(result.task_id());
    ack->set_attempt_id(result.attempt_id());
    ack->set_accepted(accepted);
    ack->set_retryable(retryable);
    ack->set_reason(reason);
    server_.sendTo(identity, env);
}

void Scheduler::HandleTaskReport(const IncomingMessage& msg) {
    const auto& result = msg.envelope.task_report().result();
    const auto found = in_flight_.find(result.task_id());
    if (found == in_flight_.end() || !Matches(found->second, msg, result.attempt_id()) ||
        found->second.run_id != result.run_id()) {
        const bool duplicate = std::any_of(
            accepted_reports_.begin(), accepted_reports_.end(), [&](const AcceptedReport& r) {
                return r.identity == msg.identity && r.session_id == msg.envelope.session_id() &&
                       r.plugin_type == msg.envelope.plugin_type() &&
                       r.task_id == result.task_id() && r.run_id == result.run_id() &&
                       r.attempt_id == result.attempt_id();
            });
        SendTaskReportAck(msg.identity, msg.envelope.plugin_type(), msg.envelope.session_id(),
                          result, duplicate, false, duplicate ? "" : "obsolete attempt");
        return;
    }
    // A negative code means no durable per-file outcome was published (or
    // execution was cancelled). Re-execute through a new assignment attempt.
    if (result.exit_code() < 0) {
        ReclaimTasks({result.task_id()});
        const bool reclaimed = in_flight_.count(result.task_id()) == 0;
        SendTaskReportAck(msg.identity, msg.envelope.plugin_type(), msg.envelope.session_id(),
                          result, false, !reclaimed, "execution needs retry");
        return;
    }
    // Once received, this attempt cannot be reclaimed while its commit retries.
    if (!found->second.received) found->second.result = result;
    found->second.received = true;
    CommitReport(result.task_id());
}

bool Scheduler::CommitReport(const std::string& id) {
    const auto found = in_flight_.find(id);
    if (found == in_flight_.end() || !found->second.received) return false;
    const auto& attempt = found->second;
    bool accepted = plugins_by_type_.at(attempt.plugin_type)->processResult(attempt.result);
    if (accepted && storage_.enabled()) accepted = storage_.ReportTask(id, attempt.result);
    SendTaskReportAck(attempt.identity, attempt.plugin_type, attempt.session_id, attempt.result,
                      accepted, !accepted, accepted ? "" : "result persistence failed; retrying");
    if (!accepted) {
        LOG_WARN("result persistence failed; retrying task " << id);
        return false;
    }
    accepted_reports_.push_back({attempt.identity, attempt.session_id, attempt.plugin_type, id,
                                 attempt.run_id, attempt.attempt_id});
    if (accepted_reports_.size() > 8192) accepted_reports_.pop_front();
    in_flight_.erase(found);
    in_flight_count_.store(in_flight_.size());
    return true;
}

void Scheduler::HandleTaskAck(const IncomingMessage& msg) {
    const auto& ack = msg.envelope.task_ack();
    const auto found = in_flight_.find(ack.task_id());
    if (found == in_flight_.end() || !Matches(found->second, msg, ack.attempt_id())) return;
    found->second.acked = true;
    // Receipt is diagnostic. Dispatch and result writes are the recovery gates.
    if (storage_.enabled() && !storage_.AckTask(ack.task_id()))
        LOG_WARN("task receipt persistence failed: " << ack.task_id());
}
void Scheduler::HandleTaskReject(const IncomingMessage& msg) {
    const auto& rejected = msg.envelope.task_reject();
    const auto found = in_flight_.find(rejected.task_id());
    if (found != in_flight_.end() && Matches(found->second, msg, rejected.attempt_id()))
        ReclaimTasks({rejected.task_id()});
}
}  // namespace scheduler
