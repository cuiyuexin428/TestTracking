#include "scheduler/Scheduler.h"

#include <algorithm>
#include <filesystem>
#include <set>
#include <stdexcept>

#include "common/AtomicFile.h"
#include "common/Util.h"

namespace scheduler {
namespace {
using Json = nlohmann::json;
bool Identity(const Json& value) {
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    return !text.empty() && text.size() <= 255 && std::none_of(text.begin(), text.end(),
        [](unsigned char c) { return c < 33 || c == 127; });
}
bool Digest(const Json& value) {
    return value.is_string() && value.get_ref<const std::string&>().size() == 64 &&
        value.get_ref<const std::string&>().find_first_not_of("0123456789abcdef") == std::string::npos;
}
bool Unfinished(const std::string& status) {
    return status == "queued" || status == "planning" || status == "running" || status == "stopped";
}
Json Reject(int code, const std::string& error) {
    return {{"ok", false}, {"http_status", code}, {"error", error}};
}
Json Entry(const Json& config, const std::string& key) {
    if (config.contains("plugins") && config["plugins"].is_object() && config["plugins"].contains(key))
        return config["plugins"][key];
    return nullptr;
}
bool TargetPatch(const Json& patch) {
    static const std::set<std::string> allowed = {"expected_program_version", "expected_baseline_program_version",
        "expected_candidate_program_version", "expected_dataset_version", "stp_manifest"};
    if (!patch.is_object()) return false;
    for (const auto& field : patch.items())
        if (!allowed.count(field.key()) || !field.value().is_string() ||
            field.value().get_ref<const std::string&>().find('\0') != std::string::npos) return false;
    return true;
}
}

bool Scheduler::SavePluginRollouts(const Json& state) const {
    if (config_path_.empty()) return false;
    auto path = std::filesystem::u8path(config_path_);
    path += ".rollouts.json";
    return common::WriteAtomicFile(path, Json{{"version", 1}, {"plugins", state}}.dump(2));
}

void Scheduler::LoadPluginRollouts() {
    if (config_path_.empty()) return;
    auto path = std::filesystem::u8path(config_path_);
    path += ".rollouts.json";
    if (!std::filesystem::exists(path)) return;
    std::string bytes;
    if (!common::ReadCompleteFile(path, &bytes, 16 * 1024 * 1024))
        throw std::runtime_error("cannot read plugin rollout state");
    const auto saved = Json::parse(bytes, nullptr, false);
    if (!saved.is_object() || saved.value("version", 0) != 1 || !saved.contains("plugins") || !saved["plugins"].is_object())
        throw std::runtime_error("invalid plugin rollout state");
    for (const auto& item : saved["plugins"].items()) {
        const auto& value = item.value();
        if (!Identity(item.key()) || !value.is_object() || !Identity(value.at("rollout_id")) ||
            !Identity(value.at("configuration_key")) || !Digest(value.at("target_id")) ||
            !value.at("base_configuration").is_object() || !value.at("target_configuration").is_object() ||
            !TargetPatch(value.at("target_patch")) || !value.at("boundary").is_object() ||
            !value.at("abandoned").is_object() ||
            !(value.at("admission_target") == "" || Digest(value.at("admission_target"))))
            throw std::runtime_error("invalid plugin rollout entry");
        const auto phase = value.at("phase").get<std::string>();
        if (phase != "waiting" && phase != "switching" && phase != "completed" && phase != "cancelled")
            throw std::runtime_error("invalid plugin rollout phase");
        for (const auto& run : value["boundary"].items())
            if (!Identity(run.key()) || !run.value().is_string()) throw std::runtime_error("invalid rollout boundary");
    }
    plugin_rollouts_ = saved["plugins"];
}

std::string Scheduler::RolloutKey(const std::string& type) const {
    if (plugin_rollouts_.contains(type)) return type;
    for (const auto& plugin : plugins_) {
        if (plugin.instance->pluginType() != type) continue;
        for (const auto& item : plugin_rollouts_.items())
            if (item.value().at("configuration_key") == plugin.configuration_key) return item.key();
    }
    return type;
}

bool Scheduler::RolloutPending(const std::string& type) const {
    const auto key = RolloutKey(type);
    return plugin_rollouts_.contains(key) &&
        (plugin_rollouts_.at(key).at("phase") == "waiting" || plugin_rollouts_.at(key).at("phase") == "switching");
}

bool Scheduler::RolloutSwitching(const std::string& type) const {
    const auto key = RolloutKey(type);
    return plugin_rollouts_.contains(key) && plugin_rollouts_.at(key).at("phase") == "switching";
}

bool Scheduler::RolloutBoundaryContains(const std::string& type, const std::string& id) const {
    return RolloutPending(type) && plugin_rollouts_.at(RolloutKey(type)).at("boundary").contains(id);
}

bool Scheduler::RolloutAutoResume(const std::string& type, const std::string& id) const {
    if (!RolloutBoundaryContains(type, id)) return false;
    const auto& saved = plugin_rollouts_.at(RolloutKey(type));
    return saved.contains("resume_after_restart") && saved.at("resume_after_restart").contains(id);
}

void Scheduler::PrepareRolloutShutdown() {
    auto next = plugin_rollouts_;
    bool changed = false;
    for (const auto& run : active_runs_) {
        if (!RolloutBoundaryContains(run.second.plugin_type, run.second.run_id) || run.second.stopping) continue;
        next[run.second.plugin_type]["resume_after_restart"][run.second.run_id] = true;
        changed = true;
    }
    if (changed) {
        if (!SavePluginRollouts(next)) throw std::runtime_error("cannot preserve active rollout batches for shutdown");
        plugin_rollouts_.swap(next);
    }
}

void Scheduler::RestoreRolloutRunsWithoutStorage() {
    // Without the framework database an in-progress attempt cannot be
    // reconciled after a crash. Preserve it as a visible stopped blocker rather
    // than silently treating an empty new process as a completed old boundary.
    for (const auto& item : plugin_rollouts_.items()) {
        if (item.value().at("phase") != "waiting") continue;
        const auto& saved = item.value();
        const auto definitions = saved.value("definitions", Json::object());
        const auto order = saved.value("run_order", Json::array());
        for (const auto& id : order) {
            const auto run_id = id.get<std::string>();
            if (!saved.at("boundary").contains(run_id) || !definitions.contains(run_id)) continue;
            const auto status = saved.at("boundary").at(run_id).get<std::string>();
            if (!Unfinished(status)) continue;
            const auto& fields = definitions.at(run_id);
            RunDefinition run{run_id, item.key(), fields.at("dataset_version").get<std::string>(),
                fields.at("params").get<std::string>(), fields.at("work_fingerprint").get<std::string>(),
                fields.at("force_rerun").get<bool>(), fields.at("force_rerun_reason").get<std::string>(),
                fields.at("rerun_of").get<std::string>(), fields.at("worker_ids").get<std::vector<std::string>>(),
                fields.at("execution_fingerprint").get<std::string>()};
            run_definitions_[run_id] = run;
            run_states_[run_id] = {item.key(), run.work_fingerprint, status == "queued" ? "queued" : "stopped"};
            run_order_.push_back(run_id);
            if (status == "queued" && plugins_by_type_.count(item.key())) queued_runs_[plugins_by_type_.at(item.key())].push_back(run);
        }
    }
    for (const auto& run : run_states_) UpdateRolloutRun(run.first, run.second.status);
    for (const auto& plugin : plugins_by_type_) StartNextRun(plugin.second);
}

bool Scheduler::RolloutStopped(const std::string& type) const {
    if (!RolloutPending(type)) return false;
    for (const auto& value : plugin_rollouts_.at(RolloutKey(type)).at("boundary"))
        if (value == "stopped") return true;
    return false;
}

bool Scheduler::RolloutBlocksClientReservation(const std::string& id) const {
    for (const auto& item : plugin_rollouts_.items()) {
        if (item.value().at("phase") != "waiting") continue;
        bool unfinished = false;
        for (const auto& run : item.value().at("boundary")) unfinished = unfinished || Unfinished(run.get<std::string>());
        if (!unfinished) continue;
        const auto agent = agents_.find(id);
        if (agent != agents_.end() && std::find(agent->second.plugin_keys.begin(), agent->second.plugin_keys.end(),
                item.value().at("configuration_key").get<std::string>()) != agent->second.plugin_keys.end()) return true;
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (const auto& worker : clients_)
            if (worker.second.physical_client_id == id && RolloutKey(worker.second.info.plugin_type) == item.key()) return true;
    }
    return false;
}

bool Scheduler::RolloutAllowsWorker(const std::string& type, const std::string& identity) const {
    if (RolloutSwitching(type)) return false;
    const auto key = RolloutKey(type);
    if (!plugin_rollouts_.contains(key)) return true;
    const auto target = plugin_rollouts_.at(key).at("admission_target").get<std::string>();
    if (target.empty()) return true;
    std::lock_guard<std::mutex> lock(clients_mutex_);
    const auto found = clients_.find(identity);
    return found != clients_.end() && found->second.deployment_target_id == target;
}

bool Scheduler::RolloutAllowsDeployment(const std::string& type, const Json& request, const Json& patch) const {
    const auto key = RolloutKey(type);
    if (!RolloutPending(type)) return !request.contains("rollout_id") &&
        (!plugin_rollouts_.contains(key) || plugin_rollouts_.at(key).at("admission_target") == "");
    const auto& rollout = plugin_rollouts_.at(key);
    return rollout.at("phase") == "switching" && request.contains("rollout_id") &&
        request.at("rollout_id") == rollout.at("rollout_id") && patch == rollout.at("target_patch");
}

void Scheduler::UpdateRolloutRun(const std::string& id, const std::string& status) {
    auto next = plugin_rollouts_;
    bool changed = false;
    for (auto& value : next) {
        if (value.at("phase") != "waiting" || !value.at("boundary").contains(id) || value["boundary"][id] == status) continue;
        value["boundary"][id] = status;
        if (status != "stopped" && value.contains("resume_after_restart")) value["resume_after_restart"].erase(id);
        changed = true;
    }
    if (changed) {
        if (!SavePluginRollouts(next)) throw std::runtime_error("cannot persist rollout batch progress");
        plugin_rollouts_.swap(next);
    }
}

Json Scheduler::PluginRolloutStatus(const std::string& type) {
    if (!plugin_rollouts_.contains(type)) return Reject(404, "plugin rollout not found");
    const auto& saved = plugin_rollouts_.at(type);
    Json boundary = Json::array(), blockers = Json::array();
    std::unordered_map<std::string, std::string> pending;
    std::vector<Storage::RunRecord> durable;
    if (storage_.enabled() && !storage_.ReadUnfinishedRuns(type, durable))
        return Reject(503, "cannot verify durable rollout batches");
    std::vector<std::string> boundary_ids;
    for (const auto& run : saved.at("boundary").items()) boundary_ids.push_back(run.key());
    std::unordered_map<std::string, std::string> persisted;
    if (storage_.enabled() && !storage_.ReadRolloutRunStatuses(type, boundary_ids, persisted))
        return Reject(503, "cannot verify durable rollout boundary states");
    for (const auto& run : saved.at("boundary").items())
        if (storage_.enabled() && !persisted.count(run.key())) pending[run.key()] = "recovery_required";
    for (const auto& run : durable) pending[run.run_id] = run.status;
    for (const auto& run : run_states_)
        if (run.second.plugin_type == type && Unfinished(run.second.status)) pending[run.first] = run.second.status;
    for (const auto& run : saved.at("boundary").items()) {
        boundary.push_back(run.key());
        if (!storage_.enabled() && !run_states_.count(run.key()) && Unfinished(run.value().get<std::string>()))
            pending[run.key()] = run.value().get<std::string>();
    }
    auto next = plugin_rollouts_;
    bool changed = false;
    if (storage_.enabled()) {
        for (const auto& run : persisted) {
            if (next[type]["boundary"][run.first] != run.second) {
                next[type]["boundary"][run.first] = run.second;
                changed = true;
            }
        }
    }
    if (changed) {
        if (!SavePluginRollouts(next)) return Reject(503, "cannot persist reconciled rollout boundary");
        plugin_rollouts_.swap(next);
    }
    for (const auto& run : pending)
        blockers.push_back({{"run_id", run.first}, {"status", run.second},
            {"reason", run.second == "stopped" ? "resume or explicitly abandon this stopped batch" : "waiting for the old batch to finish"}});
    for (const auto& task : in_flight_)
        if (task.second.plugin_type == type)
            blockers.push_back({{"run_id", task.second.run_id}, {"status", "result_pending"}, {"task_id", task.first}});
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (const auto& worker : clients_)
            if (worker.second.info.plugin_type == type && worker.second.info.heartbeat.status() == common::Heartbeat::BUSY)
                blockers.push_back({{"run_id", ""}, {"status", "client_busy"}, {"client_id", worker.second.physical_client_id},
                    {"reason", "waiting for task result acknowledgement"}});
    }
    const auto& current = plugin_rollouts_.at(type);
    return {{"ok", true}, {"plugin_type", type}, {"rollout_id", current.at("rollout_id")},
        {"target_id", current.at("target_id")}, {"phase", current.at("phase")}, {"boundary", boundary},
        {"blockers", blockers}, {"ready_to_switch", current.at("phase") == "waiting" && blockers.empty()},
        {"admission_target", current.at("admission_target")}};
}

Json Scheduler::HandlePluginRollout(const Json& request) {
    if (!request.contains("plugin_type") || !Identity(request["plugin_type"])) return Reject(400, "plugin_type is required");
    const auto type = request["plugin_type"].get<std::string>();
    const auto action = request.at("action").get<std::string>();
    if (action == "get_plugin_rollout") {
        if (request.contains("rollout_id") && plugin_rollouts_.contains(type) &&
            plugin_rollouts_[type]["rollout_id"] != request["rollout_id"]) {
            for (const auto& previous : plugin_rollouts_[type].value("superseded", Json::array()))
                if (previous.at("rollout_id") == request["rollout_id"])
                    return {{"ok", true}, {"plugin_type", type}, {"rollout_id", previous.at("rollout_id")},
                        {"phase", "superseded"}, {"target_id", previous.at("target_id")},
                        {"superseded_by", plugin_rollouts_[type].at("rollout_id")}};
            return Reject(404, "rollout token not found");
        }
        return PluginRolloutStatus(type);
    }
    if (!request.contains("rollout_id") || !Identity(request["rollout_id"])) return Reject(400, "rollout_id is required");
    const auto id = request["rollout_id"].get<std::string>();
    auto next = plugin_rollouts_;
    if (action == "begin_plugin_rollout") {
        if (RolloutKey(type) != type)
            return Reject(409, "managed plugin runtime type changed; migrate its existing deployment target before publishing another update");
        if (!request.contains("target_id") || !Digest(request["target_id"]) ||
            !request.contains("target_patch") || !TargetPatch(request["target_patch"]))
            return Reject(400, "target_id must be SHA-256 and target_patch must contain supported deployment fields");
        if (next.contains(type) && next[type]["rollout_id"] == id) {
            if (next[type]["target_id"] != request["target_id"] || next[type]["target_patch"] != request["target_patch"])
                return Reject(409, "rollout token already identifies a different immutable target");
            return PluginRolloutStatus(type);
        }
        const bool replacement = next.contains(type) && (next[type]["phase"] == "switching" || next[type]["phase"] == "completed") &&
            request.contains("replaces_rollout_id") && request["replaces_rollout_id"] == next[type]["rollout_id"];
        if (request.contains("replaces_rollout_id") && !replacement) return Reject(409, "replacement requires the current switching or completed rollout token");
        if (RolloutPending(type) && !replacement) return Reject(409, "plugin already has a pending rollout");
        if (replacement) {
            auto status = PluginRolloutStatus(type);
            if (!status.value("ok", false)) return status;
            if (!status.at("blockers").empty()) return Reject(409, "replacement cannot discard unfinished batches or reports");
        }
        if (PluginUnderMaintenance(type)) return Reject(409, "plugin is reserved for maintenance");
        const auto loaded = std::find_if(plugins_.begin(), plugins_.end(), [&](const LoadedPlugin& p) { return p.instance->pluginType() == type; });
        if (loaded == plugins_.end()) return Reject(409, "plugin is not enabled and loaded");
        if (config_path_.empty() || !request.contains("config_path") || !request["config_path"].is_string() ||
            !request.contains("config_revision") || !Digest(request["config_revision"])) return Reject(400, "managed configuration path and revision are required");
        std::error_code error;
        const auto requested = std::filesystem::u8path(request["config_path"].get<std::string>());
        const auto path = std::filesystem::u8path(config_path_);
        if (!requested.is_absolute() || !std::filesystem::equivalent(path, requested, error) || error)
            return Reject(409, "rollout configuration is not loaded by this scheduler");
        std::string bytes;
        if (!common::ReadCompleteFile(path, &bytes, 16 * 1024 * 1024)) return Reject(503, "cannot read scheduler configuration");
        if (common::Sha256Hex(bytes) != request["config_revision"] || Json::parse(bytes, nullptr, false) != effective_configuration_)
            return Reject(409, "scheduler configuration changed or has unapplied settings");
        std::vector<Storage::RunRecord> durable;
        if (storage_.enabled() && !storage_.ReadUnfinishedRuns(type, durable)) return Reject(503, "cannot capture durable rollout boundary");
        Json boundary = Json::object();
        for (const auto& run : durable) boundary[run.run_id] = run.status;
        for (const auto& run : run_states_)
            if (run.second.plugin_type == type && Unfinished(run.second.status)) boundary[run.first] = run.second.status;
        Json definitions = Json::object(), order = Json::array();
        for (const auto& run_id : run_order_) {
            if (!boundary.contains(run_id) || !run_definitions_.count(run_id)) continue;
            const auto& run = run_definitions_.at(run_id);
            order.push_back(run_id);
            definitions[run_id] = {{"dataset_version", run.dataset_version}, {"params", run.params},
                {"work_fingerprint", run.work_fingerprint}, {"force_rerun", run.force_rerun},
                {"force_rerun_reason", run.force_rerun_reason}, {"rerun_of", run.rerun_of},
                {"worker_ids", run.worker_ids}, {"execution_fingerprint", run.execution_fingerprint}};
        }
        const auto base = Entry(effective_configuration_, loaded->configuration_key);
        auto target = base;
        target.update(request["target_patch"]);
        const auto admitted = next.contains(type) ? next[type].value("admission_target", std::string()) : std::string();
        auto superseded = next.contains(type) ? next[type].value("superseded", Json::array()) : Json::array();
        if (replacement) superseded.push_back({{"rollout_id", next[type]["rollout_id"]}, {"target_id", next[type]["target_id"]}});
        next[type] = {{"rollout_id", id}, {"target_id", request["target_id"]}, {"phase", replacement ? "switching" : "waiting"},
            {"configuration_key", loaded->configuration_key}, {"base_configuration", base}, {"target_configuration", target},
            {"target_patch", request["target_patch"]}, {"boundary", boundary},
            {"admission_target", replacement ? request["target_id"].get<std::string>() : admitted},
            {"abandoned", Json::object()}, {"superseded", superseded}, {"definitions", definitions},
            {"run_order", order}, {"resume_after_restart", Json::object()}};
    } else {
        if (!next.contains(type) || next[type]["rollout_id"] != id) return Reject(409, "rollout ownership token does not match");
        auto& value = next[type];
        if (action == "cancel_plugin_rollout") {
            if (value["phase"] == "cancelled") return PluginRolloutStatus(type);
            if (value["phase"] != "waiting") return Reject(409, "rollout has already entered switching and cannot be cancelled");
            value["phase"] = "cancelled";
        } else if (action == "activate_plugin_rollout") {
            if (value["phase"] == "switching" || value["phase"] == "completed") return PluginRolloutStatus(type);
            if (value["phase"] != "waiting") return Reject(409, "rollout is not waiting");
            auto status = PluginRolloutStatus(type);
            next[type]["boundary"] = plugin_rollouts_[type]["boundary"];
            if (!status.value("ok", false)) return status;
            if (!status["ready_to_switch"].get<bool>()) {
                status["ok"] = false; status["http_status"] = 409; status["error"] = "old batches or results are unfinished";
                return status;
            }
            if (Entry(effective_configuration_, value["configuration_key"].get<std::string>()) != value["base_configuration"])
                return Reject(409, "original plugin configuration changed during rollout wait");
            std::string bytes;
            if (!common::ReadCompleteFile(std::filesystem::u8path(config_path_), &bytes, 16 * 1024 * 1024))
                return Reject(503, "cannot read rollout configuration before switching");
            if (Entry(Json::parse(bytes, nullptr, false), value["configuration_key"].get<std::string>()) != value["base_configuration"])
                return Reject(409, "original plugin configuration has unapplied changes");
            value["phase"] = "switching";
            value["admission_target"] = value["target_id"];
        } else if (action == "finish_plugin_rollout") {
            if (value["phase"] == "completed") return PluginRolloutStatus(type);
            if (value["phase"] != "switching") return Reject(409, "rollout has not entered switching");
            if (Entry(effective_configuration_, value["configuration_key"].get<std::string>()) != value["target_configuration"])
                return Reject(409, "server target configuration has not taken effect");
            std::string bytes;
            if (!common::ReadCompleteFile(std::filesystem::u8path(config_path_), &bytes, 16 * 1024 * 1024))
                return Reject(503, "cannot read rollout configuration before completion");
            if (Entry(Json::parse(bytes, nullptr, false), value["configuration_key"].get<std::string>()) != value["target_configuration"])
                return Reject(409, "target plugin configuration differs on disk");
            bool ready = false;
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                const auto timeout = std::chrono::milliseconds(std::max(1, config_.get("server.agent_timeout_ms", 60000)));
                for (const auto& worker : clients_) {
                    if (!worker.second.info.registered || worker.second.info.plugin_type != type ||
                        worker.second.deployment_target_id != value["target_id"] ||
                        client_reservations_.count(worker.second.physical_client_id)) continue;
                    const auto agent = agents_.find(worker.second.physical_client_id);
                    if (agent != agents_.end() && agent->second.seen_since_start && agent->second.state == "ready" &&
                        Clock::now() - agent->second.last_seen < timeout) ready = true;
                }
            }
            if (!ready) return Reject(409, "no confirmed ready worker for the new target");
            value["phase"] = "completed";
        } else if (action == "resume_rollout_run" || action == "abandon_rollout_run") {
            if (value["phase"] != "waiting" || !request.contains("run_id") || !Identity(request["run_id"]))
                return Reject(409, "stopped batch operations require a waiting rollout and run_id");
            const auto run_id = request["run_id"].get<std::string>();
            if (!value["boundary"].contains(run_id)) return Reject(409, "run is outside this rollout boundary");
            if (action == "abandon_rollout_run" && value["abandoned"].contains(run_id) && value["boundary"][run_id] == "cancelled")
                return PluginRolloutStatus(type);
            std::vector<Storage::RunRecord> durable;
            if (storage_.enabled() && !storage_.ReadUnfinishedRuns(type, durable)) return Reject(503, "cannot verify stopped batch");
            const auto recorded = std::find_if(durable.begin(), durable.end(), [&](const Storage::RunRecord& r) { return r.run_id == run_id; });
            const auto memory = run_states_.find(run_id);
            auto status = recorded != durable.end() ? recorded->status :
                (memory != run_states_.end() ? memory->second.status : value["boundary"][run_id].get<std::string>());
            if (recorded == durable.end() && storage_.enabled()) {
                std::unordered_map<std::string, std::string> states;
                if (!storage_.ReadRolloutRunStatuses(type, {run_id}, states)) return Reject(503, "cannot reconcile stopped batch operation");
                if (!states.count(run_id)) return Reject(409, "stopped batch is missing from durable history");
                status = states.at(run_id);
            }
            if (action == "resume_rollout_run" && status == "running" && plugins_by_type_.count(type)) {
                const auto active = active_runs_.find(plugins_by_type_.at(type));
                if (active != active_runs_.end() && active->second.run_id == run_id) {
                    UpdateRolloutRun(run_id, "running");
                    return PluginRolloutStatus(type);
                }
            }
            if (action == "abandon_rollout_run" && status == "cancelled" && value["abandoned"].contains(run_id)) {
                UpdateRolloutRun(run_id, "cancelled");
                if (plugins_by_type_.count(type)) StartNextRun(plugins_by_type_.at(type));
                return PluginRolloutStatus(type);
            }
            if (status != "stopped") return Reject(409, "only stopped batches can be resumed or abandoned");
            if (action == "abandon_rollout_run") {
                if (!request.contains("reason") || !request["reason"].is_string() ||
                    request["reason"].get_ref<const std::string&>().find_first_not_of(" \t\r\n") == std::string::npos ||
                    request["reason"].get_ref<const std::string&>().size() > 2048)
                    return Reject(400, "an abandonment reason is required (at most 2048 bytes)");
                // Record the decision before the database cancellation. A lost
                // reply is retried with the same run and token; never erase work.
                value["abandoned"][run_id] = {{"reason", request["reason"]}, {"operator_id", request.value("operator_id", std::string())}};
                if (!SavePluginRollouts(next)) return Reject(503, "cannot record batch abandonment");
                plugin_rollouts_ = next;
                if (storage_.enabled() && !storage_.AbandonStoppedRun(run_id, type)) return Reject(503, "cannot persist stopped batch abandonment");
                if (memory != run_states_.end()) memory->second.status = "cancelled";
                value["boundary"][run_id] = "cancelled";
            } else {
                const auto plugin = plugins_by_type_.find(type);
                if (plugin == plugins_by_type_.end() || active_runs_.count(plugin->second)) return Reject(409, "plugin must have no active batch before resuming");
                RunDefinition definition;
                if (recorded != durable.end()) definition = {recorded->run_id, recorded->plugin_type, recorded->dataset_version,
                    recorded->params, recorded->work_fingerprint, recorded->force_rerun, recorded->force_rerun_reason,
                    recorded->rerun_of, recorded->worker_ids, recorded->execution_fingerprint};
                else if (run_definitions_.count(run_id)) definition = run_definitions_.at(run_id);
                else return Reject(409, "stopped batch has no recoverable definition; restore its durable state or explicitly abandon it");
                if (!ConfigurationMatches(definition)) return Reject(409, "restore the stopped batch's original configuration");
                if (!plugin->second->restore(run_id)) return Reject(409, "plugin could not restore this stopped batch");
                if (storage_.enabled() && !storage_.RestoreRun(run_id, type)) {
                    plugin->second->cancel();
                    return Reject(503, "cannot persist resumed batch");
                }
                active_runs_[plugin->second] = {run_id, type, false, definition.worker_ids};
                active_run_count_.store(active_runs_.size());
                run_states_[run_id] = {type, definition.work_fingerprint, "running"};
                run_definitions_[run_id] = definition;
                value["boundary"][run_id] = "running";
                SendWake(type);
            }
        } else return Reject(400, "unknown plugin rollout action");
    }
    if (!SavePluginRollouts(next)) return Reject(503, "cannot persist plugin rollout state");
    plugin_rollouts_.swap(next);
    if (action == "abandon_rollout_run" && plugins_by_type_.count(type)) StartNextRun(plugins_by_type_.at(type));
    return PluginRolloutStatus(type);
}
}  // namespace scheduler
