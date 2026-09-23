#include "scheduler/Scheduler.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

#include "common/AtomicFile.h"
#include "common/Util.h"

namespace scheduler {
namespace {
bool Revision(const std::string& value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}
bool Identity(const nlohmann::json& value) {
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    return !text.empty() && text.size() <= 255 && std::none_of(text.begin(), text.end(),
        [](unsigned char c) { return c < 33 || c == 127; });
}
nlohmann::json Entry(const nlohmann::json& config, const std::string& key) {
    if (config.contains("plugins") && config["plugins"].is_object() && config["plugins"].contains(key))
        return config["plugins"][key];
    return nullptr;
}
}

bool Scheduler::SavePluginMaintenance(
        const std::unordered_map<std::string, PluginMaintenance>& entries) const {
    auto path = std::filesystem::u8path(config_path_);
    path += ".maintenance.json";
    nlohmann::json state = {{"version", 1}, {"plugins", nlohmann::json::object()}};
    for (const auto& item : entries) {
        const auto& value = item.second;
        state["plugins"][item.first] = {{"reservation_id", value.reservation_id},
            {"original_revision", value.original_revision}, {"committed_revision", value.committed_revision},
            {"configuration_key", value.configuration_key},
            {"committed_configuration", value.committed_configuration}};
    }
    return common::WriteAtomicFile(path, state.dump(2));
}

void Scheduler::LoadPluginMaintenance() {
    if (config_path_.empty()) return;
    auto path = std::filesystem::u8path(config_path_);
    path += ".maintenance.json";
    if (!std::filesystem::exists(path)) return;
    std::string bytes;
    if (!common::ReadCompleteFile(path, &bytes, 16 * 1024 * 1024))
        throw std::runtime_error("cannot read plugin maintenance state");
    const auto state = nlohmann::json::parse(bytes, nullptr, false);
    if (!state.is_object() || state.value("version", 0) != 1 ||
        !state.contains("plugins") || !state["plugins"].is_object())
        throw std::runtime_error("invalid plugin maintenance state");
    for (const auto& item : state["plugins"].items()) {
        const auto& value = item.value();
        if (!Identity(item.key()) || !value.is_object() || !value.contains("reservation_id") ||
            !Identity(value["reservation_id"]) || !value.contains("configuration_key") ||
            !Identity(value["configuration_key"]))
            throw std::runtime_error("invalid plugin maintenance reservation");
        PluginMaintenance entry{value.at("reservation_id").get<std::string>(),
            value.at("original_revision").get<std::string>(), value.at("committed_revision").get<std::string>(),
            value.at("configuration_key").get<std::string>(), value.at("committed_configuration")};
        if (!Revision(entry.original_revision) ||
            (!entry.committed_revision.empty() && !Revision(entry.committed_revision)))
            throw std::runtime_error("invalid plugin maintenance revision");
        // Only an explicit successful install followed by loading its exact
        // plugin configuration completes maintenance. Interrupted preparation
        // remains reserved across either process restarting.
        if (!entry.committed_revision.empty() &&
            Entry(effective_configuration_, entry.configuration_key) == entry.committed_configuration)
            continue;
        plugin_maintenance_.emplace(item.key(), std::move(entry));
    }
    if (plugin_maintenance_.size() != state["plugins"].size() && !SavePluginMaintenance(plugin_maintenance_))
        throw std::runtime_error("cannot finish plugin maintenance after restart");
}

bool Scheduler::MaintenanceConfigurationMatches(const nlohmann::json& disk) const {
    auto desired = disk;
    auto effective = effective_configuration_;
    // Several distinct plugins can be installed before one restart. Only the
    // explicitly committed maintenance entries may differ from loaded settings.
    for (const auto& item : plugin_maintenance_) {
        const auto& entry = item.second;
        if (entry.committed_revision.empty() ||
            Entry(disk, entry.configuration_key) != entry.committed_configuration) continue;
        if (desired.contains("plugins") && desired["plugins"].is_object())
            desired["plugins"].erase(entry.configuration_key);
        if (effective.contains("plugins") && effective["plugins"].is_object())
            effective["plugins"].erase(entry.configuration_key);
    }
    return desired == effective;
}

bool Scheduler::PluginUnderMaintenance(const std::string& type) const {
    if (plugin_maintenance_.count(type)) return true;
    // A replacement can expose a different runtime type after a restart before
    // the installer has finished. The reservation still owns its configuration
    // entry and must block that replacement until the transaction is resolved.
    for (const auto& loaded : plugins_) {
        if (loaded.instance->pluginType() != type) continue;
        for (const auto& item : plugin_maintenance_)
            if (item.second.configuration_key == loaded.configuration_key) return true;
    }
    return false;
}

nlohmann::json Scheduler::HandlePluginMaintenance(const nlohmann::json& request) {
    const auto reject = [](int status, const std::string& error) -> nlohmann::json {
        return {{"ok", false}, {"error", error}, {"http_status", status}};
    };
    for (const auto* key : {"plugin_type", "reservation_id"})
        if (!request.contains(key) || !Identity(request[key])) return reject(400, std::string("invalid ") + key);
    if (!request.contains("config_path") || !request["config_path"].is_string() ||
        !request.contains("config_revision") || !request["config_revision"].is_string() ||
        !Revision(request["config_revision"].get<std::string>()))
        return reject(400, "config_path and SHA-256 config_revision are required");
    if (config_path_.empty()) return reject(409, "scheduler has no managed configuration file");
    const auto path = std::filesystem::u8path(config_path_);
    const auto requested = std::filesystem::u8path(request["config_path"].get<std::string>());
    std::error_code error;
    if (!requested.is_absolute() || !std::filesystem::equivalent(path, requested, error) || error)
        return reject(409, "config_path is not the configuration loaded by this scheduler");
    auto lock_path = path;
    lock_path += ".apply.lock";
    common::FilePublicationLock publication(lock_path);
    if (!publication.acquired()) return reject(503, "cannot lock scheduler configuration");
    std::string bytes;
    if (!common::ReadCompleteFile(path, &bytes, 16 * 1024 * 1024))
        return reject(503, "cannot read scheduler configuration");
    const auto revision = request["config_revision"].get<std::string>();
    if (common::Sha256Hex(bytes) != revision) return reject(409, "scheduler configuration revision changed");
    const auto disk = nlohmann::json::parse(bytes, nullptr, false);
    if (!disk.is_object()) return reject(422, "invalid scheduler configuration");
    const auto requested_type = request["plugin_type"].get<std::string>();
    const auto token = request["reservation_id"].get<std::string>();
    std::string type = requested_type, key = requested_type;
    bool resolved = false;
    // Accept an existing configuration key as an alias, but reserve and check
    // work by the actual runtime type. Fail closed if a key names one plugin
    // while also being the runtime type of a different loaded plugin.
    for (const auto& loaded : plugins_) {
        const auto runtime_type = loaded.instance->pluginType();
        if (requested_type != runtime_type && requested_type != loaded.configuration_key) continue;
        if (resolved && key != loaded.configuration_key)
            return reject(409, "ambiguous plugin type or configuration key: " + requested_type);
        type = runtime_type;
        key = loaded.configuration_key;
        resolved = true;
    }
    // Keep the original durable identity when a replacement changes its type.
    // Either the original type, its configuration alias or the currently loaded
    // replacement must refer to the same reservation and ownership token.
    for (const auto& item : plugin_maintenance_) {
        if (requested_type != item.first && requested_type != item.second.configuration_key &&
            (!resolved || key != item.second.configuration_key)) continue;
        if (resolved && key != item.second.configuration_key)
            return reject(409, "ambiguous plugin maintenance identity: " + requested_type);
        type = item.first;
        key = item.second.configuration_key;
        resolved = true;
    }
    const auto found = plugin_maintenance_.find(type);
    if (RolloutPending(type)) return reject(409, "plugin has a pending global deployment update");
    const bool begin = request["action"] == "begin_plugin_maintenance";
    if (found != plugin_maintenance_.end() && found->second.reservation_id != token)
        return reject(409, "plugin is already reserved for maintenance: " + type);
    auto next = plugin_maintenance_;
    bool pending_restart = false;
    if (begin) {
        if (found != plugin_maintenance_.end()) {
            if (found->second.original_revision != revision)
                return reject(409, "maintenance reservation was created for another configuration revision");
            return {{"ok", true}, {"reservation_id", token}, {"plugin_type", type},
                {"pending_restart", !found->second.committed_revision.empty()}};
        }
        if (!MaintenanceConfigurationMatches(disk))
            return reject(409, "scheduler configuration has unrelated unapplied changes");
        for (const auto& run : run_states_)
            if (run.second.plugin_type == type && (run.second.status == "queued" || run.second.status == "planning" ||
                run.second.status == "running" || run.second.status == "stopped"))
                return reject(409, "plugin has unfinished work: " + type);
        const auto plugin = plugins_by_type_.find(type);
        if ((plugin != plugins_by_type_.end() && (active_runs_.count(plugin->second) ||
            (queued_runs_.count(plugin->second) && !queued_runs_.at(plugin->second).empty()))) ||
            std::any_of(in_flight_.begin(), in_flight_.end(), [&](const auto& item) { return item.second.plugin_type == type; }))
            return reject(409, "plugin has active, queued or in-flight work: " + type);
        if (storage_.enabled()) {
            bool unfinished = false;
            if (!storage_.HasUnfinishedRuns(type, unfinished)) return reject(503, "cannot verify durable unfinished work");
            if (unfinished) return reject(409, "plugin has durable unfinished work: " + type);
        }
        next.emplace(type, PluginMaintenance{token, revision, {}, key, nullptr});
    } else {
        if (!request.contains("committed") || !request["committed"].is_boolean())
            return reject(400, "committed must be a boolean");
        const bool committed = request["committed"].get<bool>();
        if (found == plugin_maintenance_.end()) {
            if (committed && (revision != startup_configuration_revision_ || disk != effective_configuration_))
                return reject(409, "maintenance reservation is missing and configuration has not been loaded");
            return {{"ok", true}, {"reservation_id", token}, {"plugin_type", type}, {"pending_restart", false}};
        }
        auto& entry = next.at(type);
        if (committed) {
            if (!entry.committed_revision.empty() && entry.committed_revision != revision)
                return reject(409, "maintenance was already committed with another revision");
            entry.committed_revision = revision;
            entry.committed_configuration = Entry(disk, entry.configuration_key);
            pending_restart = true;
        } else {
            if (!entry.committed_revision.empty() || revision != entry.original_revision)
                return reject(409, "restore the original configuration before releasing maintenance");
            if (Entry(effective_configuration_, entry.configuration_key) == Entry(disk, entry.configuration_key)) {
                next.erase(type);
            } else {
                // If this scheduler restarted into the replacement before the
                // installer rolled back, it must reload the restored DLL too.
                entry.committed_revision = revision;
                entry.committed_configuration = Entry(disk, entry.configuration_key);
                pending_restart = true;
            }
        }
    }
    if (!SavePluginMaintenance(next)) return reject(503, "cannot persist plugin maintenance state");
    plugin_maintenance_.swap(next);
    return {{"ok", true}, {"reservation_id", token}, {"plugin_type", type}, {"pending_restart", pending_restart}};
}
}  // namespace scheduler
