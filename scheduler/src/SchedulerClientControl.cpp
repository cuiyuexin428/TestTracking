#include "scheduler/Scheduler.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <stdexcept>

#include "common/AtomicFile.h"

namespace scheduler {
namespace {
long long NowMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool ValidIdentity(const nlohmann::json& value) {
    if (!value.is_string()) return false;
    const auto text = value.get<std::string>();
    return !text.empty() && text.size() <= 255 &&
        !std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); }) &&
        !std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; });
}

nlohmann::json Error(const std::string& message, int code = 400) {
    return {{"ok", false}, {"error", message}, {"http_status", code}};
}
}  // namespace

void Scheduler::LoadClientControl() {
    // Production always supplies its resolved config path. Embedders without a
    // path cannot publish durable deployment reservations into an arbitrary cwd.
    if (config_path_.empty()) return;
    client_control_path_ = (std::filesystem::u8path(config_path_).parent_path() /
                            "runtime" / "client_control.json").u8string();
    const auto state_path = std::filesystem::u8path(client_control_path_);
    if (!std::filesystem::exists(state_path)) return;
    std::string bytes;
    if (!common::ReadCompleteFile(state_path, &bytes, 4 * 1024 * 1024))
        throw std::runtime_error("cannot read client control state: " + client_control_path_);
    const auto saved = nlohmann::json::parse(bytes, nullptr, false);
    if (!saved.is_object() || saved.value("version", 0) != 1 ||
        !saved.contains("reservations") || !saved["reservations"].is_object())
        throw std::runtime_error("invalid client control state: " + client_control_path_);
    std::unordered_map<std::string, ClientReservation> restored;
    for (auto it = saved["reservations"].begin(); it != saved["reservations"].end(); ++it) {
        const auto& value = it.value();
        if (!ValidIdentity(it.key()) || !value.is_object() ||
            !value.contains("reservation_id") || !ValidIdentity(value["reservation_id"]) ||
            !value.contains("committed") || !value["committed"].is_boolean() ||
            !value.contains("expires_at_ms") || !value["expires_at_ms"].is_number_integer())
            throw std::runtime_error("invalid client reservation: " + client_control_path_);
        ClientReservation reservation{value["reservation_id"].get<std::string>(),
                                      value["committed"].get<bool>(),
                                      value["expires_at_ms"].get<long long>()};
        if (reservation.committed || reservation.expires_at_ms > NowMilliseconds())
            restored[it.key()] = std::move(reservation);
    }
    client_reservations_ = std::move(restored);
    if (saved.contains("agents")) {
        if (!saved["agents"].is_object())
            throw std::runtime_error("invalid agent registry: " + client_control_path_);
        for (auto it = saved["agents"].begin(); it != saved["agents"].end(); ++it) {
            const auto& value = it.value();
            if (!ValidIdentity(it.key()) || !value.is_object())
                throw std::runtime_error("invalid agent registry entry: " + client_control_path_);
            agents_[it.key()] = {value.at("instance_id").get<std::string>(),
                value.at("hostname").get<std::string>(), value.at("state").get<std::string>(),
                value.at("agent_version").get<std::string>(), value.at("active_revision").get<std::string>(),
                value.at("plugin_keys").get<std::vector<std::string>>(), {},
                value.at("update_window_open").get<bool>(), value.at("last_seen_at_ms").get<long long>(), false};
            agents_.at(it.key()).capabilities = value.value("capabilities", std::vector<std::string>{});
        }
    }
}

bool Scheduler::SaveClientControl(
    const std::unordered_map<std::string, ClientReservation>& reservations) const {
    if (client_control_path_.empty()) return false;
    nlohmann::json saved = {{"version", 1}, {"reservations", nlohmann::json::object()}};
    for (const auto& entry : reservations)
        saved["reservations"][entry.first] = {{"reservation_id", entry.second.reservation_id},
            {"committed", entry.second.committed}, {"expires_at_ms", entry.second.expires_at_ms}};
    saved["agents"] = nlohmann::json::object();
    for (const auto& entry : agents_) {
        const auto& agent = entry.second;
        saved["agents"][entry.first] = {{"instance_id", agent.instance_id}, {"hostname", agent.hostname},
            {"state", agent.state}, {"agent_version", agent.agent_version}, {"active_revision", agent.active_revision},
            {"plugin_keys", agent.plugin_keys}, {"update_window_open", agent.update_window_open},
            {"last_seen_at_ms", agent.last_seen_at_ms}, {"capabilities", agent.capabilities}};
    }
    return common::WriteAtomicFile(std::filesystem::u8path(client_control_path_), saved.dump(2));
}

void Scheduler::ExpireClientReservations() {
    std::vector<std::string> expired;
    auto next = client_reservations_;
    for (auto it = next.begin(); it != next.end();) {
        if (!it->second.committed && it->second.expires_at_ms <= NowMilliseconds()) {
            expired.push_back(it->first);
            it = next.erase(it);
        } else ++it;
    }
    if (expired.empty()) return;
    if (!SaveClientControl(next))
        throw std::runtime_error("cannot persist expired client reservations");
    client_reservations_.swap(next);
    WakePhysicalClients(expired);
}

bool Scheduler::PhysicalClientBusy(const std::string& client_id) const {
    for (const auto& entry : in_flight_)
        if (entry.second.physical_client_id == client_id) return true;
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (const auto& entry : clients_)
        if (entry.second.physical_client_id == client_id &&
            entry.second.info.heartbeat.status() == common::Heartbeat::BUSY) return true;
    return false;
}

void Scheduler::WakePhysicalClients(const std::vector<std::string>& client_ids) {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (const auto& entry : clients_) {
        if (std::find(client_ids.begin(), client_ids.end(), entry.second.physical_client_id) == client_ids.end())
            continue;
        common::Envelope message;
        message.set_type(common::WAKE);
        message.set_plugin_type(entry.second.info.plugin_type);
        message.set_session_id(entry.second.session_id);
        message.mutable_wake();
        server_.sendTo(entry.first, message);
    }
}

void Scheduler::AppendAgentStatus(nlohmann::json& result) const {
    result["agents"] = nlohmann::json::array();
    result["client_control_path"] = client_control_path_;
    std::set<std::string> ids;
    for (const auto& entry : agents_) ids.insert(entry.first);
    for (const auto& entry : client_reservations_) ids.insert(entry.first);
    const auto timeout = std::max(1, config_.get("server.agent_timeout_ms", 60000));
    for (const auto& id : ids) {
        const auto found = agents_.find(id);
        const AgentEntry empty{};
        const auto& agent = found == agents_.end() ? empty : found->second;
        const auto age = found == agents_.end() ? static_cast<long long>(timeout) : agent.seen_since_start ?
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - agent.last_seen).count() :
            std::max(0LL, NowMilliseconds() - agent.last_seen_at_ms);
        const auto reservation = client_reservations_.find(id);
        result["agents"].push_back({{"client_id", id}, {"instance_id", agent.instance_id},
            {"hostname", agent.hostname}, {"state", agent.state},
            {"online", found != agents_.end() && agent.seen_since_start && age < timeout}, {"last_seen_age_ms", age},
            {"busy", PhysicalClientBusy(id)}, {"plugin_keys", agent.plugin_keys},
            {"agent_version", agent.agent_version}, {"active_revision", agent.active_revision},
            {"capabilities", agent.capabilities},
            {"update_window_open", agent.update_window_open},
            {"reservation_id", reservation == client_reservations_.end() ? "" : reservation->second.reservation_id},
            {"reservation_committed", reservation != client_reservations_.end() && reservation->second.committed}});
    }
}

nlohmann::json Scheduler::HandleClientControl(const nlohmann::json& request) {
    ExpireClientReservations();
    const auto action = request["action"].get<std::string>();
    if (action == "agent_heartbeat") {
        for (const auto* field : {"client_id", "instance_id"})
            if (!request.contains(field) || !ValidIdentity(request[field]))
                return Error(std::string(field) + " must be a nonempty identity of at most 255 bytes");
        for (const auto* field : {"hostname", "state", "agent_version", "active_revision"})
            if (request.contains(field) && !request[field].is_string())
                return Error(std::string(field) + " must be a string");
        const auto state = request.value("state", std::string());
        if (state != "unconfigured" && state != "ready" && state != "starting" &&
            state != "updating" && state != "error")
            return Error("state must be unconfigured, ready, starting, updating or error");
        if (!request.contains("plugin_keys") || !request["plugin_keys"].is_array())
            return Error("plugin_keys must be an array of plugin identities");
        for (const auto& key : request["plugin_keys"])
            if (!ValidIdentity(key)) return Error("plugin_keys must contain nonempty plugin identities");
        if (request.contains("update_window_open") && !request["update_window_open"].is_boolean())
            return Error("update_window_open must be a boolean");
        if (request.contains("capabilities")) {
            if (!request["capabilities"].is_array()) return Error("capabilities must be an array");
            for (const auto& value : request["capabilities"])
                if (!ValidIdentity(value)) return Error("capabilities must contain identities");
        }
        const auto id = request["client_id"].get<std::string>();
        const auto previous = agents_.find(id);
        const bool existed = previous != agents_.end();
        const AgentEntry old = existed ? previous->second : AgentEntry{};
        agents_[id] = {request["instance_id"].get<std::string>(), request.value("hostname", id), state,
            request.value("agent_version", std::string()), request.value("active_revision", std::string()),
            request["plugin_keys"].get<std::vector<std::string>>(), Clock::now(),
            request.value("update_window_open", false), NowMilliseconds(), true};
        agents_.at(id).capabilities = request.value("capabilities", std::vector<std::string>{});
        if (!SaveClientControl(client_reservations_)) {
            if (existed) agents_[id] = old;
            else agents_.erase(id);
            return Error("cannot persist agent registry", 503);
        }
        return {{"ok", true}, {"client_id", id}};
    }
    if (!request.contains("reservation_id") || !ValidIdentity(request["reservation_id"]))
        return Error("reservation_id must be a nonempty identity of at most 255 bytes");
    if (request.contains("require_update_window_open") && !request["require_update_window_open"].is_boolean())
        return Error("require_update_window_open must be a boolean");
    if (!request.contains("client_ids") || !request["client_ids"].is_array() || request["client_ids"].empty())
        return Error("client_ids must be a nonempty array of client identities");
    std::set<std::string> ids;
    for (const auto& value : request["client_ids"]) {
        if (!ValidIdentity(value)) return Error("client_ids must contain nonempty client identities");
        ids.insert(value.get<std::string>());
    }
    const auto token = request["reservation_id"].get<std::string>();
    nlohmann::json unavailable = nlohmann::json::array();
    const auto timeout = std::chrono::milliseconds(std::max(1, config_.get("server.agent_timeout_ms", 60000)));
    for (const auto& id : ids) {
        const auto reservation = client_reservations_.find(id);
        std::string reason;
        if (reservation != client_reservations_.end() && reservation->second.reservation_id != token)
            reason = "reserved";
        else if (action == "commit_reservation" && reservation == client_reservations_.end())
            reason = "reservation_missing";
        else if (action == "reserve_clients" && reservation == client_reservations_.end()) {
            const auto agent = agents_.find(id);
            if (agent == agents_.end() || !agent->second.seen_since_start ||
                Clock::now() - agent->second.last_seen >= timeout)
                reason = "offline";
            else if (PhysicalClientBusy(id)) reason = "busy";
            else if (RolloutBlocksClientReservation(id)) reason = "old_plugin_batches_pending";
            else if (agent->second.state == "updating") reason = "updating";
            else if (request.value("require_update_window_open", false) && !agent->second.update_window_open)
                reason = "waiting_window";
            // A registered Agent can repair workers whose startup is blocked
            // by the server's newly bound program version. The authoritative
            // busy checks above still protect every plugin on that machine.
            // A closed update window may receive a deployment. Its reservation
            // deliberately blocks task dispatch until the Agent can activate it.
        }
        if (!reason.empty()) unavailable.push_back({{"client_id", id}, {"reason", reason}});
    }
    if (!unavailable.empty()) {
        auto result = Error("one or more clients are unavailable for this deployment operation", 409);
        result["unavailable"] = std::move(unavailable);
        return result;
    }
    auto next = client_reservations_;
    const auto expires = NowMilliseconds() + std::max(1, config_.get("server.deployment_reservation_timeout_ms", 60000));
    for (const auto& id : ids) {
        if (action == "release_clients") next.erase(id);
        else if (action == "commit_reservation") {
            next.at(id).committed = true;
            next.at(id).expires_at_ms = 0;
        } else {
            auto& reservation = next[id];
            reservation.reservation_id = token;
            if (!reservation.committed) reservation.expires_at_ms = expires;
        }
    }
    if (!SaveClientControl(next)) return Error("cannot persist client control state", 503);
    client_reservations_.swap(next);
    if (action == "release_clients") WakePhysicalClients({ids.begin(), ids.end()});
    const bool committed = action != "release_clients" &&
        std::all_of(ids.begin(), ids.end(), [this](const std::string& id) {
            return client_reservations_.at(id).committed;
        });
    return {{"ok", true}, {"reservation_id", token}, {"client_ids", ids},
            {"committed", committed}, {"released", action == "release_clients"}};
}
}  // namespace scheduler
