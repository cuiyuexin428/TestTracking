#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "client/TaskExecutor.h"
#include "common/BlockingQueue.h"
#include "common/Config.h"
#include "common/DynamicLibrary.h"
#include "common/zmq_helper.h"
#include "communication.pb.h"

namespace client {
struct ClientPluginDeleter {
    common::DestroyClientPluginFn destroy = nullptr;
    void operator()(common::WorkerPlugin* plugin) const {
        if (plugin && destroy) destroy(plugin);
    }
};

// The calling thread owns the socket, session and task counters. Workers only
// publish completion events. Pausing execution never pauses session maintenance.
class Client {
   public:
    explicit Client(common::Config& config, std::shared_ptr<std::atomic<bool>> stop_signal = {});
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    void start();
    void stop();  // Thread-safe request; never joins a worker.

   private:
    struct LoadedPlugin {
        std::unique_ptr<common::DynamicLibrary> library;
        std::unique_ptr<common::WorkerPlugin, ClientPluginDeleter> instance;
    };
    using TaskKey = std::pair<std::string, std::string>;
    struct PendingReport {
        common::TaskResult result;
        std::chrono::steady_clock::time_point retry_at{};
    };
    bool LoadPlugins();
    void PrepareRegistration();
    void RefreshRegistration();
    void SendRegistration(std::chrono::steady_clock::time_point now);
    void HandleRegistration(const common::Envelope& envelope);
    void BeginRegistration();
    void RunStateMachine();
    void HandleEnvelope(const common::Envelope& env);
    void HandleTask(const common::TaskCommand& command, const std::string& plugin_type);
    void HandleStop(const std::string& run_id, const std::string& reason);
    void SendHeartbeat();
    bool SendRequestTask(int count);
    bool SendEnvelope(const common::Envelope& envelope);
    bool QueueControl(common::Envelope envelope);
    void FlushOutgoing();
    void CollectCompletedResults();
    void RetryReports(std::chrono::steady_clock::time_point now);
    void CancelExecution();
    void PollExecutor();

    common::Config& config_;
    common::ZmqDealer dealer_;
    std::vector<LoadedPlugin> plugins_;
    common::WorkerPlugin* plugin_ = nullptr;
    TaskExecutor executor_;
    std::shared_ptr<std::atomic<bool>> stop_requested_;
    bool silent_ = false;
    bool draining_executor_ = false;
    bool executor_started_ = false;
    bool registered_ = false;
    bool has_registered_ = false;
    std::deque<common::Envelope> outgoing_;
    std::map<TaskKey, common::Task> active_tasks_;
    std::map<TaskKey, PendingReport> pending_reports_;
    int outstanding_requests_ = 0;
    int task_slots_ = 1;
    common::BlockingQueue<common::TaskResult> completed_results_;
    common::Envelope registration_;
    int registration_attempts_ = 0;
    std::chrono::steady_clock::time_point last_heartbeat_;
    std::chrono::steady_clock::time_point last_server_message_;
    std::chrono::steady_clock::time_point next_registration_;
    std::chrono::steady_clock::time_point registration_started_;
    std::chrono::steady_clock::time_point next_request_;
    std::chrono::steady_clock::time_point request_sent_at_;
    std::chrono::milliseconds backoff_;
    std::string worker_id_;
    std::string registration_id_;
    std::string session_id_;
    std::string ip_;
    std::chrono::milliseconds heartbeat_interval_;
    std::chrono::milliseconds poll_timeout_;
    std::chrono::milliseconds request_backoff_initial_;
    std::chrono::milliseconds request_backoff_max_;
    int register_timeout_ms_ = 5000;
    int register_max_attempts_ = 5;
    int request_timeout_ms_ = 5000;
    int report_retry_ms_ = 500;
    size_t outgoing_limit_ = 64;
};
}  // namespace client
