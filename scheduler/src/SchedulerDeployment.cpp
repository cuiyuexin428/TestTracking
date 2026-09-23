#include "scheduler/Scheduler.h"

#include <algorithm>
#include <filesystem>
#include <set>

#include "common/AtomicFile.h"
#include "common/Logger.h"
#include "common/Manifest.h"
#include "common/Util.h"

namespace scheduler {

Scheduler::PluginIdentity Scheduler::DescribePlugin(const std::string& type,
        const nlohmann::json& configuration, const std::string& binary_sha256) {
    nlohmann::json versions = nlohmann::json::object();
    for (const auto* key : {"expected_program_version", "expected_baseline_program_version",
                            "expected_candidate_program_version"})
        if (configuration.contains(key) && configuration[key].is_string())
            versions[key] = configuration[key];
    const nlohmann::json deployment = {{"plugin_type", type},
        {"plugin_binary_sha256", binary_sha256}, {"expected_program_versions", versions}};
    PluginIdentity result{common::Sha256Hex(deployment.dump()),
        common::Sha256Hex(configuration.dump()), configuration.value("stp_manifest", std::string())};
    if (type == "coverage")
        result.duplicate_protection = versions.contains("expected_program_version") &&
            !versions["expected_program_version"].get<std::string>().empty();
    else if (type == "stpcompare")
        result.duplicate_protection = versions.contains("expected_baseline_program_version") &&
            versions.contains("expected_candidate_program_version") &&
            !versions["expected_baseline_program_version"].get<std::string>().empty() &&
            !versions["expected_candidate_program_version"].get<std::string>().empty();
    return result;
}

nlohmann::json Scheduler::ApplyPluginDeployment(const nlohmann::json& request) {
    const auto reject = [](int status, const std::string& error) -> nlohmann::json {
        return {{"ok", false}, {"error", error}, {"http_status", status}};
    };
    for (const auto* field : {"config_path", "config_revision"})
        if (!request.contains(field) || !request[field].is_string() ||
            request[field].get_ref<const std::string&>().empty())
            return reject(400, std::string(field) + " must be a nonempty string");
    if (!request.contains("plugins") || !request["plugins"].is_object() || request["plugins"].empty())
        return reject(400, "plugins must be a nonempty object keyed by loaded plugin type");
    const auto expected_revision = request["config_revision"].get<std::string>();
    if (expected_revision.size() != 64 || expected_revision.find_first_not_of("0123456789abcdef") != std::string::npos)
        return reject(400, "config_revision must be a SHA-256 revision");
    if (config_path_.empty()) return reject(409, "scheduler has no managed configuration file");
    const auto path = std::filesystem::u8path(config_path_);
    const auto requested_path = std::filesystem::u8path(request["config_path"].get<std::string>());
    std::error_code path_error;
    if (!requested_path.is_absolute() || !std::filesystem::equivalent(path, requested_path, path_error) || path_error)
        return reject(409, "config_path is not the configuration loaded by this scheduler");

    // HTTP commands and CLI runs share the processing queue. Keep preparation,
    // durable publication and activation inside this one lifecycle barrier.
    auto lock_path = path;
    lock_path += ".apply.lock";
    common::FilePublicationLock publication(lock_path);
    if (!publication.acquired()) return reject(503, "cannot acquire the scheduler configuration publication lock");
    std::string original;
    if (!common::ReadCompleteFile(path, &original, 16 * 1024 * 1024))
        return reject(503, "cannot read the scheduler configuration file");
    if (common::Sha256Hex(original) != expected_revision)
        return reject(409, "scheduler configuration changed; preview the deployment again");
    auto proposed = nlohmann::json::parse(original, nullptr, false);
    if (!proposed.is_object()) return reject(422, "scheduler configuration is not a JSON object");
    if (proposed != effective_configuration_)
        return reject(409, "scheduler configuration has unapplied changes; online deployment cannot apply unrelated settings");

    const std::set<std::string> allowed = {"expected_program_version", "expected_baseline_program_version",
        "expected_candidate_program_version", "stp_manifest", "expected_dataset_version"};
    struct Candidate {
        std::size_t index;
        std::string type;
        std::unique_ptr<common::TaskPlugin, PluginDeleter> instance;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(request["plugins"].size());
    auto deployment_identities = deployment_identities_;
    auto configuration_identities = configuration_identities_;
    auto manifest_paths = manifest_paths_;
    auto duplicate_protection = duplicate_protection_enabled_;
    std::vector<std::pair<std::size_t, std::string>> changes;

    // Validate the entire batch before initializing any replacement instance.
    for (const auto& item : request["plugins"].items()) {
        const auto& type = item.key();
        if (!RolloutAllowsDeployment(type, request, item.value()))
            return reject(409, "plugin rollout requires its switching owner and immutable target patch: " + type);
        if (PluginUnderMaintenance(type))
            return reject(409, "plugin is reserved for maintenance: " + type);
        if (!item.value().is_object() || item.value().empty())
            return reject(400, "each plugin deployment patch must be a nonempty object");
        for (const auto& field : item.value().items()) {
            if (!allowed.count(field.key()) || !field.value().is_string() ||
                field.value().get_ref<const std::string&>().find('\0') != std::string::npos)
                return reject(400, "unsupported deployment field or non-string value: " + type + "." + field.key());
        }
        const auto found = plugins_by_type_.find(type);
        if (found == plugins_by_type_.end()) return reject(409, "plugin is not enabled and loaded: " + type);
        const auto loaded = std::find_if(plugins_.begin(), plugins_.end(), [&](const LoadedPlugin& plugin) {
            return plugin.instance.get() == found->second;
        });
        if (loaded == plugins_.end()) return reject(503, "loaded plugin metadata is unavailable: " + type);
        auto& entry = proposed["plugins"][loaded->configuration_key];
        if (!entry.is_object() || !entry.value("enabled", false))
            return reject(409, "plugin is not enabled: " + type);
        auto next = entry;
        for (const auto& field : item.value().items()) next[field.key()] = field.value();
        if (next == entry) continue;
        if (active_runs_.count(found->second) ||
            (queued_runs_.count(found->second) && !queued_runs_.at(found->second).empty()) ||
            std::any_of(in_flight_.begin(), in_flight_.end(), [&](const auto& task) {
                return task.second.plugin_type == type;
            })) return reject(409, "plugin has active, queued or in-flight work: " + type);
        for (const auto& run : run_states_) {
            const auto& state = run.second;
            if (state.plugin_type == type && (state.status == "queued" || state.status == "planning" ||
                state.status == "running" || state.status == "stopped"))
                return reject(409, "plugin has unfinished work that requires its original deployment: " + type);
        }
        if (storage_.enabled()) {
            bool unfinished = false;
            if (!storage_.HasUnfinishedRuns(type, unfinished))
                return reject(503, "cannot verify durable unfinished work for plugin: " + type);
            if (unfinished) return reject(409, "plugin has durable unfinished work: " + type);
        }
        entry = std::move(next);
        changes.emplace_back(static_cast<std::size_t>(loaded - plugins_.begin()), type);
    }
    if (changes.empty())
        return {{"ok", true}, {"configuration_revision", expected_revision}, {"online_applied", true}};

    for (const auto& change : changes) {
        const auto& loaded = plugins_[change.first];
        const auto& entry = proposed["plugins"][loaded.configuration_key];
        const auto& type = change.second;
        const auto identity = DescribePlugin(type, entry, loaded.binary_sha256);
        if (!identity.manifest.empty()) {
            std::vector<common::ManifestEntry> manifest;
            if (!common::ReadManifest(identity.manifest, &manifest))
                return reject(422, "deployment manifest is invalid, empty or unreadable: " + type);
        } else if (entry.contains("stp_manifest") || type == "coverage" || type == "stpcompare") {
            return reject(422, "deployment manifest must not be empty: " + type);
        }
        auto create = loaded.library->get<common::CreatePluginFn>("CreatePlugin");
        auto destroy = loaded.library->get<common::DestroyPluginFn>("DestroyPlugin");
        try {
            std::unique_ptr<common::TaskPlugin, PluginDeleter> candidate(create(), PluginDeleter{destroy});
            if (!candidate || !candidate->init(entry.dump()) || candidate->pluginType() != type)
                return reject(422, "replacement plugin initialization failed: " + type);
            candidates.push_back({change.first, type, std::move(candidate)});
        } catch (const std::exception& error) {
            LOG_ERROR("deployment initialization failed for " << type << ": " << error.what());
            return reject(422, "replacement plugin initialization failed: " + type);
        } catch (...) {
            return reject(422, "replacement plugin initialization failed: " + type);
        }
        deployment_identities.at(type) = identity.deployment;
        configuration_identities.at(type) = identity.configuration;
        manifest_paths.at(type) = identity.manifest;
        duplicate_protection.at(type) = identity.duplicate_protection;
    }

    const auto encoded = proposed.dump(2) + '\n';
    const auto revision = common::Sha256Hex(encoded);
    // Prepare the response and every allocating replacement before publication.
    nlohmann::json response = {{"ok", true}, {"configuration_revision", revision}, {"online_applied", true}};
    std::string current;
    if (!common::ReadCompleteFile(path, &current, 16 * 1024 * 1024) || current != original)
        return reject(409, "scheduler configuration changed during deployment preparation");
    std::unique_lock<std::mutex> clients_lock(clients_mutex_);
    auto backup = path;
    backup += ".bak";
    if (!common::WriteAtomicFile(backup, original) || !common::WriteAtomicFile(path, encoded))
        return reject(503, "cannot publish deployment configuration; the active deployment is unchanged");

    // Publication is the commit point. A crash after this point restarts with
    // the new configuration; there was no unfinished work for changed plugins.
    // No DLL is unloaded or replaced, and other plugins retain their instances.
    for (auto& candidate : candidates) {
        auto* old = plugins_[candidate.index].instance.get();
        queued_runs_.erase(old);  // Only empty queues can reach this point.
        plugins_by_type_.at(candidate.type) = candidate.instance.get();
        plugins_[candidate.index].instance.swap(candidate.instance);
        for (auto client = clients_.begin(); client != clients_.end();) {
            if (client->second.info.plugin_type == candidate.type) client = clients_.erase(client);
            else ++client;
        }
    }
    effective_configuration_.swap(proposed);
    deployment_identities_.swap(deployment_identities);
    configuration_identities_.swap(configuration_identities);
    manifest_paths_.swap(manifest_paths);
    duplicate_protection_enabled_.swap(duplicate_protection);
    clients_lock.unlock();
    LOG_INFO("plugin deployment configuration applied online: " << revision);
    return response;
}
}  // namespace scheduler
