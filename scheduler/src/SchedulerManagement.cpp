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
                             {"queued_runs", nlohmann::json::array()},
                             {"unfinished_runs", nlohmann::json::array()},
                             {"clients", nlohmann::json::array()}, {"in_flight", in_flight_.size()}};
    for (const auto& pair : run_states_) {
        const auto& state = pair.second;
        if (state.status == "queued" || state.status == "planning" ||
            state.status == "running" || state.status == "stopped")
            result["unfinished_runs"].push_back({{"run_id", pair.first},
                {"plugin_type", state.plugin_type}, {"status", state.status}});
    }
    for (const auto& pair : active_runs_)
        result["active_runs"].push_back({{"run_id", pair.second.run_id},
                                          {"plugin_type", pair.second.plugin_type},
                                          {"stopping", pair.second.stopping},
                                          {"worker_ids", pair.second.worker_ids}});
    for (const auto& queue : queued_runs_)
        for (const auto& run : queue.second)
            result["queued_runs"].push_back(
                {{"run_id", run.run_id},
                 {"plugin_type", run.plugin_type},
                 {"dataset_version", run.dataset_version},
                 {"status", "queued"},
                 {"force_rerun", run.force_rerun},
                 {"force_rerun_reason", run.force_rerun_reason},
                 {"rerun_of", run.rerun_of},
                 {"worker_ids", run.worker_ids},
                 {"blocked", !run.blocked_reason.empty()},
                 {"blocked_reason", run.blocked_reason}});
    for (const auto& client : clients()) {
        const auto& heartbeat = client.heartbeat;
        std::string physical_client_id = client.identity, plugin_key = client.plugin_type;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            const auto entry = clients_.find(client.identity);
            if (entry != clients_.end()) {
                physical_client_id = entry->second.physical_client_id;
                plugin_key = entry->second.plugin_key;
            }
        }
        nlohmann::json tasks = nlohmann::json::array();
        for (const auto& task : in_flight_) {
            if (task.second.identity == client.identity)
                tasks.push_back({{"task_id", task.first}, {"run_id", task.second.run_id},
                                 {"attempt_id", task.second.attempt_id}, {"acked", task.second.acked}});
        }
        result["clients"].push_back({{"worker_id", client.identity}, {"plugin_type", client.plugin_type},
            {"physical_client_id", physical_client_id}, {"plugin_key", plugin_key},
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
    if (request.contains("force_rerun") && !request["force_rerun"].is_boolean())
        return invalid("force_rerun must be a boolean");
    if (request.contains("force_rerun_reason") && !request["force_rerun_reason"].is_string())
        return invalid("force_rerun_reason must be a string");
    if (request.contains("worker_ids")) {
        if (!request["worker_ids"].is_array()) return invalid("worker_ids must be an array of strings");
        for (const auto& value : request["worker_ids"]) {
            if (!value.is_string()) return invalid("worker_ids must contain nonempty strings");
            const auto worker = value.get<std::string>();
            if (worker.empty() || worker.size() > 255 ||
                std::all_of(worker.begin(), worker.end(), [](unsigned char c) { return std::isspace(c); }) ||
                std::any_of(worker.begin(), worker.end(), [](unsigned char c) { return c < 32 || c == 127; }))
                return invalid("worker_ids must contain nonempty identities of at most 255 bytes");
        }
    }
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
    if (command.kind != ControlCommand::Kind::Run &&
        (request.contains("force_rerun") || request.contains("force_rerun_reason") || request.contains("worker_ids")))
        return invalid("force rerun fields and worker_ids are only valid for run commands");
    if (request.contains("worker_ids")) {
        command.worker_ids = request["worker_ids"].get<std::vector<std::string>>();
        std::sort(command.worker_ids.begin(), command.worker_ids.end());
        command.worker_ids.erase(std::unique(command.worker_ids.begin(), command.worker_ids.end()),
                                 command.worker_ids.end());
    }
    command.force_rerun = request.value("force_rerun", false);
    command.force_rerun_reason = request.value("force_rerun_reason", std::string());
    const bool reason_blank = std::all_of(command.force_rerun_reason.begin(),
                                          command.force_rerun_reason.end(),
                                          [](unsigned char c) { return std::isspace(c); });
    if (command.force_rerun && (command.force_rerun_reason.empty() || reason_blank))
        return invalid("force_rerun_reason is required when force_rerun is true");
    if (!command.force_rerun && request.contains("force_rerun_reason"))
        return invalid("force_rerun_reason requires force_rerun=true");
    if (request.contains("params")) command.params = request["params"].is_string()
        ? request["params"].get<std::string>() : request["params"].dump();
    return SubmitDetailed(std::move(command), true);
}
}  // namespace scheduler
