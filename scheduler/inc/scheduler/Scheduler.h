#pragma once

#include "common/BlockingQueue.h"
#include "common/Config.h"
#include "common/DynamicLibrary.h"
#include "common/EventManager.h"
#include "common/TaskPlugin.h"
#include "communication.pb.h"
#include "scheduler/Server.h"
#include "scheduler/Storage.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
// and runs one processing thread that handles messages and executes user
// commands. The framework only routes by plugin_type; business payloads are
// passed through to plugins untouched. Clients pull tasks themselves:
// REGISTER -> validateClient, REQUEST_TASK -> TASK/NO_TASK,
// TASK_REPORT -> processResult.
class Scheduler {
public:
    explicit Scheduler(common::Config& config);
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    void start();
    void stop();

    // Queue a run creation for the plugin with the given task type.
    bool createRun(const std::string& plugin_type, const std::string& dataset_version,
                   const std::string& params);

    // Stop the active run of the given plugin type: send STOP to its clients
    // and reclaim their in-flight tasks.
    bool stopRun(const std::string& plugin_type);

    // Send WAKEUP to clients (all when plugin_type is empty). Used to resume
    // clients that entered the silent state after STOP.
    void wakeupClients(const std::string& plugin_type);

    // Snapshot of the current client list.
    std::vector<common::ClientInfo> clients() const;

    size_t pluginCount() const;

    size_t inFlightCount() const;

    // Enabled plugin types (read-only after startup).
    std::vector<std::string> pluginTypes() const;

    // Number of currently active runs (safe to call from any thread).
    size_t activeRunCount() const {
        return active_run_count_.load(std::memory_order_relaxed);
    }

private:
    struct ClientEntry {
        common::ClientInfo info;
        std::chrono::steady_clock::time_point last_seen;
        std::unordered_set<std::string> registered_plugins;  // REGISTER accepted per plugin_type
        std::string session_id;
    };

    struct InFlight {
        std::string identity;
        std::string session_id;
        std::string plugin_type;
        std::chrono::steady_clock::time_point sent_at;
        bool acked = false;  // client confirmed receipt via TASK_ACK
    };

    struct RunContext {
        std::string run_id;
        std::string plugin_type;
    };

    struct LoadedPlugin {
        std::unique_ptr<common::DynamicLibrary> library;
        // Declaration order matters: the plugin instance must be destroyed
        // before the library is unloaded (virtual calls go into the DLL).
        std::unique_ptr<common::TaskPlugin, PluginDeleter> instance;
    };

    struct CreateRunCommand {
        std::string plugin_type;
        std::string dataset_version;
        std::string params;
    };

    struct StopRunCommand {
        std::string plugin_type;
    };

    std::atomic<size_t> active_run_count_{0};

    bool LoadPlugins();
    void RunProcessing();
    void HandleMessage(const IncomingMessage& msg);
    void HandleRegister(const IncomingMessage& msg);
    void HandleRequestTask(const IncomingMessage& msg);
    void HandleTaskReport(const IncomingMessage& msg);
    void HandleTaskAck(const IncomingMessage& msg);
    void HandleTaskReject(const IncomingMessage& msg);
    void HandleCreateRun(const CreateRunCommand& command);
    void HandleStopRun(const StopRunCommand& command);
    void ReclaimClientTasks(const std::string& identity,
                            const std::string& session_id = std::string());
    bool IsCurrentSession(const IncomingMessage& msg) const;
    void TouchClient(const std::string& identity, const std::string& plugin_type);
    bool UpdateClient(const std::string& identity, const std::string& plugin_type,
                      const common::Heartbeat& heartbeat);
    std::vector<common::ClientInfo> SnapshotClients() const;
    void RunTimeoutChecks();
    void SendRegisterResult(const std::string& identity, const std::string& plugin_type,
                            bool plugin_found, bool accepted, const std::string& reason,
                            const std::string& reply_payload,
                            const std::string& session_id = std::string(),
                            const std::string& registration_id = std::string());
    void SendTasks(common::TaskPlugin* plugin, const std::string& identity,
                   const std::vector<common::Task>& tasks, const std::string& session_id);
    void SendNoTask(const std::string& identity, const std::string& plugin_type,
                    const std::string& session_id);

    common::Config& config_;
    Server server_;
    Storage storage_;
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
    common::BlockingQueue<StopRunCommand> stop_commands_;
};

}  // namespace scheduler
