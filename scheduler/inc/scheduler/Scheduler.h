#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/BlockingQueue.h"
#include "common/Config.h"
#include "common/DynamicLibrary.h"
#include "common/TaskPlugin.h"
#include "scheduler/Server.h"
#include "scheduler/Storage.h"

namespace scheduler {
class ManagementServer;
struct PluginDeleter {
    common::DestroyPluginFn destroy = nullptr;
    void operator()(common::TaskPlugin* plugin) const {
        if (plugin && destroy) destroy(plugin);
    }
};

// Network liveness runs independently. Every plugin and durable state operation
// runs on one worker. FIFO commands therefore form lifecycle barriers.
class Scheduler {
   public:
    explicit Scheduler(common::Config& config, std::string config_path = {});
    ~Scheduler();
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    void start();
    void stop();
    // Return the actual command outcome, rather than queue acceptance.
    bool createRun(const std::string&, const std::string&, const std::string&);
    bool stopRun(const std::string&);
    bool wakeupClients(const std::string&);
    std::vector<common::ClientInfo> clients() const;
    size_t pluginCount() const;
    size_t inFlightCount() const;
    std::vector<std::string> pluginTypes() const;
    size_t activeRunCount() const { return active_run_count_.load(); }
    // The local HTTP bridge submits work to the same queue as CLI commands.
    nlohmann::json managementStatus();
    nlohmann::json managementCommand(const nlohmann::json& request);

   private:
    using Clock = std::chrono::steady_clock;
    struct ClientEntry {
        common::ClientInfo info;
        Clock::time_point last_seen;
        std::string session_id, registration_id;
        common::ValidationResult validation;
        std::string physical_client_id, plugin_key;
    };
    struct InFlight {
        std::string identity, session_id, plugin_type, run_id, attempt_id;
        Clock::time_point sent_at;
        bool acked = false, received = false;
        common::TaskResult result;
        std::string physical_client_id;
    };
    struct AgentEntry {
        std::string instance_id, hostname, state, agent_version, active_revision;
        std::vector<std::string> plugin_keys;
        Clock::time_point last_seen{};
        bool update_window_open = false;
        long long last_seen_at_ms = 0;
        bool seen_since_start = true;
    };
    struct ClientReservation {
        std::string reservation_id;
        bool committed = false;
        long long expires_at_ms = 0;
    };
    struct RunContext {
        std::string run_id, plugin_type;
        bool stopping = false;
        std::vector<std::string> worker_ids;
    };
    struct RunDefinition {
        std::string run_id, plugin_type, dataset_version, params, work_fingerprint;
        bool force_rerun = false;
        std::string force_rerun_reason, rerun_of;
        std::vector<std::string> worker_ids;
        std::string execution_fingerprint, blocked_reason;
        Clock::time_point next_configuration_check{};
    };
    struct RunState {
        std::string plugin_type, work_fingerprint, status;
        bool all_tasks_succeeded = true;
        bool has_task_reports = false;
    };
    struct LoadedPlugin {
        std::unique_ptr<common::DynamicLibrary> library;
        std::unique_ptr<common::TaskPlugin, PluginDeleter> instance;
    };
    struct ControlCommand {
        enum class Kind { Run, Stop, Wake, Shutdown, ClientControl };
        Kind kind;
        std::string plugin_type, dataset_version, params;
        std::shared_ptr<std::promise<nlohmann::json>> completion;
        std::string run_id;
        Clock::time_point deadline = Clock::time_point::max();
        bool force_rerun = false;
        std::string force_rerun_reason;
        std::vector<std::string> worker_ids;
        nlohmann::json client_control;
    };
    struct AcceptedReport {
        std::string identity, session_id, plugin_type, task_id, run_id, attempt_id;
    };

    bool Submit(ControlCommand command);
    nlohmann::json SubmitDetailed(ControlCommand command, bool bounded = false);
    nlohmann::json BuildStatus() const;
    nlohmann::json HandleClientControl(const nlohmann::json&);
    void LoadClientControl();
    bool SaveClientControl(const std::unordered_map<std::string, ClientReservation>&) const;
    void ExpireClientReservations();
    bool PhysicalClientBusy(const std::string&) const;
    void WakePhysicalClients(const std::vector<std::string>&);
    void AppendAgentStatus(nlohmann::json&) const;
    void PublishStatus();
    bool LoadPlugins();
    void RestoreRuns();
    void RunProcessing();
    nlohmann::json HandleCommand(const ControlCommand&);
    nlohmann::json HandleCreateRun(const ControlCommand&);
    bool HandleStopRun(const std::string&);
    bool StartNextRun(common::TaskPlugin*);
    bool CancelQueuedRun(common::TaskPlugin*, const std::string&);
    std::string WorkFingerprint(const ControlCommand&) const;
    std::string ExecutionFingerprint(const std::string& plugin_type,
                                     const std::string& work_fingerprint) const;
    bool ConfigurationMatches(const RunDefinition&) const;
    void SetRunStatus(const std::string&, const std::string&);
    bool CommitStop(common::TaskPlugin*);
    bool CommitFinished(common::TaskPlugin*);
    void SendWake(const std::string&);
    void HandleMessage(const IncomingMessage&);
    void HandleHeartbeat(const IncomingMessage&);
    void HandleRegister(const IncomingMessage&);
    void HandleRequestTask(const IncomingMessage&);
    void HandleTaskReport(const IncomingMessage&);
    void HandleTaskAck(const IncomingMessage&);
    void HandleTaskReject(const IncomingMessage&);
    bool CommitReport(const std::string&);
    void ReclaimTasks(const std::vector<std::string>&);
    void ReclaimClientTasks(const std::string&, const std::string&);
    bool IsCurrentSession(const IncomingMessage&) const;
    bool IsSelectedWorker(const std::string&, const std::vector<std::string>&) const;
    bool Matches(const InFlight&, const IncomingMessage&, const std::string&) const;
    void RunTimeoutChecks();
    void SendRegisterResult(const IncomingMessage&, bool, const common::ValidationResult&,
                            const std::string&);
    void SendTaskReportAck(const std::string&, const std::string&, const std::string&,
                           const common::TaskResult&, bool, bool, const std::string&);
    void SendNoTask(const IncomingMessage&);
    bool HasInFlight(const std::string&) const;

    common::Config& config_;
    std::string config_path_, instance_id_;
    Server server_;
    Storage storage_;
    std::vector<LoadedPlugin> plugins_;
    std::unordered_map<std::string, common::TaskPlugin*> plugins_by_type_;
    std::unordered_map<std::string, std::string> deployment_identities_;
    std::unordered_map<std::string, std::string> configuration_identities_;
    std::unordered_map<std::string, std::string> manifest_paths_;
    std::unordered_map<std::string, bool> duplicate_protection_enabled_;
    std::atomic<bool> running_{false}, accepting_{false};
    std::atomic<size_t> active_run_count_{0}, in_flight_count_{0};
    std::thread processing_;
    std::unique_ptr<ManagementServer> management_;
    std::mutex submit_mutex_;
    mutable std::mutex clients_mutex_;
    mutable std::mutex status_mutex_;
    nlohmann::json status_snapshot_;
    Clock::time_point status_updated_;
    std::unordered_map<std::string, ClientEntry> clients_;
    std::unordered_map<std::string, AgentEntry> agents_;
    std::unordered_map<std::string, ClientReservation> client_reservations_;
    std::string client_control_path_;
    std::unordered_map<std::string, InFlight> in_flight_;
    std::unordered_map<common::TaskPlugin*, RunContext> active_runs_;
    std::unordered_map<common::TaskPlugin*, std::deque<RunDefinition>> queued_runs_;
    std::unordered_map<std::string, RunState> run_states_;
    std::vector<std::string> run_order_;
    std::deque<AcceptedReport> accepted_reports_;
    bool shutting_down_ = false;
    common::BlockingQueue<ControlCommand> commands_;
};
}  // namespace scheduler
