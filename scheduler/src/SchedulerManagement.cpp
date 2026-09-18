#include "scheduler/Scheduler.h"

#include <algorithm>
#include <cctype>

namespace scheduler {
namespace {
void RedactConfiguration(nlohmann::json& value) {
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            auto key = it.key();
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
            if (key == "db_conn" || key.find("password") != std::string::npos ||
                key.find("secret") != std::string::npos || key.find("token") != std::string::npos ||
                key.find("api_key") != std::string::npos)
                it.value() = "[redacted]";
            else
                RedactConfiguration(it.value());
        }
    } else if (value.is_array()) {
        for (auto& item : value) RedactConfiguration(item);
    }
}
}  // namespace

nlohmann::json Scheduler::BuildStatus() const {
    nlohmann::json result = {{"online", running_.load()}, {"instance_id", instance_id_},
                             {"config_path", config_path_}, {"plugins", pluginTypes()},
                             {"active_runs", nlohmann::json::array()},
                             {"clients", nlohmann::json::array()}, {"in_flight", in_flight_.size()}};
    for (const auto& pair : active_runs_)
        result["active_runs"].push_back({{"run_id", pair.second.run_id},
                                          {"plugin_type", pair.second.plugin_type},
                                          {"stopping", pair.second.stopping}});
    for (const auto& client : clients()) {
        const auto& heartbeat = client.heartbeat;
        nlohmann::json tasks = nlohmann::json::array();
        for (const auto& task : in_flight_) {
            if (task.second.identity == client.identity)
                tasks.push_back({{"task_id", task.first}, {"run_id", task.second.run_id},
                                 {"attempt_id", task.second.attempt_id}, {"acked", task.second.acked}});
        }
        result["clients"].push_back({{"worker_id", client.identity}, {"plugin_type", client.plugin_type},
            {"registered", client.registered}, {"status", !tasks.empty() || heartbeat.status() == common::Heartbeat::BUSY ? "busy" : "idle"},
            {"ip", heartbeat.ip()}, {"peer_address", client.peer_address},
            {"last_seen_age_ms", client.last_seen_age_ms}, {"memory_bytes", heartbeat.resource().memory_bytes()},
            {"disk_free_bytes", heartbeat.resource().disk_free_bytes()}, {"cpu_percent", heartbeat.resource().cpu_percent()},
            {"plugin_info", heartbeat.plugin_info()}, {"tasks", std::move(tasks)}});
    }
    nlohmann::json configuration = nlohmann::json::object();
    for (const auto* key : {"server", "plugins"}) {
        if (const auto* section = config_.find(key)) configuration[key] = *section;
    }
    RedactConfiguration(configuration);
    result["configuration"] = std::move(configuration);
    return result;
}

void Scheduler::PublishStatus() {
    auto status = BuildStatus();
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_snapshot_ = std::move(status);
    status_updated_ = Clock::now();
}

nlohmann::json Scheduler::managementStatus() {
    std::lock_guard<std::mutex> lock(status_mutex_);
    auto status = status_snapshot_;
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - status_updated_).count();
    status["snapshot_age_ms"] = age;
    for (auto& client : status["clients"]) client["last_seen_age_ms"] = client["last_seen_age_ms"].get<long long>() + age;
    return status;
}

nlohmann::json Scheduler::managementCommand(const nlohmann::json& request) {
    const auto invalid = [](const std::string& error) -> nlohmann::json {
        return {{"ok", false}, {"error", error}, {"http_status", 400}};
    };
    if (!request.is_object() || !request.contains("action") || !request["action"].is_string())
        return invalid("action must be a string");
    for (const auto* field : {"plugin_type", "dataset_version", "run_id"})
        if (request.contains(field) && !request[field].is_string()) return invalid(std::string(field) + " must be a string");
    if (request.contains("params") && !request["params"].is_string() && !request["params"].is_object())
        return invalid("params must be a string or object");
    const auto action = request["action"].get<std::string>();
    ControlCommand command{};
    if (action == "run") command.kind = ControlCommand::Kind::Run;
    else if (action == "stop") command.kind = ControlCommand::Kind::Stop;
    else if (action == "wakeup") command.kind = ControlCommand::Kind::Wake;
    else return invalid("action must be run, stop or wakeup");
    command.plugin_type = request.value("plugin_type", std::string());
    command.dataset_version = request.value("dataset_version", std::string());
    command.run_id = request.value("run_id", std::string());
    if (command.kind != ControlCommand::Kind::Wake && command.plugin_type.empty()) return invalid("plugin_type is required");
    if (command.kind == ControlCommand::Kind::Run && command.dataset_version.empty()) return invalid("dataset_version is required");
    if (request.contains("params")) command.params = request["params"].is_string()
        ? request["params"].get<std::string>() : request["params"].dump();
    return SubmitDetailed(std::move(command), true);
}
}  // namespace scheduler
