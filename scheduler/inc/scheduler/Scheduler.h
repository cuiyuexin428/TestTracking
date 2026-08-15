#pragma once

#include "common/BlockingQueue.h"
#include "common/Config.h"
#include "common/DynamicLibrary.h"
#include "common/EventManager.h"
#include "common/TaskPlugin.h"
#include "communication.pb.h"
#include "scheduler/Server.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace scheduler {

// Deletes a plugin instance through the DestroyPlugin factory exported by
// its DLL.
struct PluginDeleter {
    common::DestroyPluginFn destroy = nullptr;

    void operator()(common::TaskPlugin* plugin) const {
        if (plugin != nullptr && destroy != nullptr) {
            destroy(plugin);
        }
    }
};

// Orchestrator: owns the network service, loads enabled plugins from config,
// and runs one processing thread that handles messages, executes user
// commands, and performs timeout checks. All plugin calls (plan/dispatch/
// requeue/processResult) happen on that single thread, so plugins never need
// internal locking. Framework code depends only on the abstract
// common::TaskPlugin interface; concrete plugins are separate DLLs.
class Scheduler {
public:
    explicit Scheduler(common::Config& config);
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    void start();
    void stop();

    // Queue a run creation for the plugin with the given task type. The
    // planning result is reported asynchronously by the processing thread.
    bool createRun(const std::string& task_type, const std::string& dataset_version,
                   const std::string& params);

    // Snapshot of the current client list.
    std::vector<common::ClientInfo> clients() const;

    size_t pluginCount() const;

    size_t inFlightCount() const;

private:
    struct ClientEntry {
        common::ClientInfo info;
        std::chrono::steady_clock::time_point last_seen;
    };

    struct InFlight {
        std::string identity;
        std::string task_type;
    };

    struct RunContext {
        std::string run_id;
        std::string task_type;
        int wave = 0;
    };

    struct LoadedPlugin {
        std::unique_ptr<common::DynamicLibrary> library;
        // Declaration order matters: the plugin instance must be destroyed
        // before the library is unloaded (virtual calls go into the DLL).
        std::unique_ptr<common::TaskPlugin, PluginDeleter> instance;
    };

    struct CreateRunCommand {
        std::string task_type;
        std::string dataset_version;
        std::string params;
    };

    bool LoadPlugins();
    void RunProcessing();
    void HandleMessage(const IncomingMessage& msg);
    void HandleCreateRun(const CreateRunCommand& command);
    void HandleResponse(const common::Response& response);
    bool UpdateClient(const std::string& identity, const common::Heartbeat& heartbeat);
    std::vector<common::ClientInfo> SnapshotClients() const;
    void RunTimeoutChecks();
    void TriggerDispatch();
    void SendRunWave(common::TaskPlugin* plugin, const common::TaskAssignment& assignment);

    common::Config& config_;
    Server server_;
    std::vector<LoadedPlugin> plugins_;
    std::unordered_map<std::string, common::TaskPlugin*> plugins_by_type_;

    std::atomic<bool> running_{false};
    std::thread processing_;
    // Shared worker pool for async plugin result processing. Owned here so
    // stop() can join its workers before plugins are destroyed.
    std::unique_ptr<common::EventManager> event_manager_;

    mutable std::mutex clients_mutex_;
    std::unordered_map<std::string, ClientEntry> clients_;

    mutable std::mutex in_flight_mutex_;
    std::unordered_map<std::string, InFlight> in_flight_;

    // Touched only by the processing thread.
    std::unordered_map<common::TaskPlugin*, RunContext> active_runs_;

    common::BlockingQueue<CreateRunCommand> commands_;
    // Completion events signal "dispatch again" here after the plugin's
    // processResult has run, so dispatch sees up-to-date plugin state.
    common::BlockingQueue<int> dispatch_triggers_;
};

}  // namespace scheduler
