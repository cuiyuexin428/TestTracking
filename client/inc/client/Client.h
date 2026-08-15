#pragma once

#include "client/TaskExecutor.h"

#include "common/BlockingQueue.h"
#include "common/Config.h"
#include "common/DynamicLibrary.h"
#include "common/zmq_helper.h"
#include "communication.pb.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace client {

// Deletes a worker plugin instance through the DestroyClientPlugin factory
// exported by its DLL.
struct ClientPluginDeleter {
    common::DestroyClientPluginFn destroy = nullptr;

    void operator()(common::WorkerPlugin* plugin) const {
        if (plugin != nullptr && destroy != nullptr) {
            destroy(plugin);
        }
    }
};

// Client state machine (main thread drives it):
//   INIT           load plugins + REGISTER validation (no threads)
//   READY          heartbeat, receive, send REQUEST_TASK when due
//   REQUEST_TASK   heartbeat, receive; TASK -> EXECUTING, NO_TASK -> READY
//   EXECUTING      heartbeat, receive; completion -> TASK_COMPLETED,
//                  STOP -> stop executor -> READY
//   TASK_COMPLETED heartbeat, send result -> READY
//   STOPPED        shutdown
enum class ClientState {
    kInit,
    kReady,
    kRequesting,
    kExecuting,
    kTaskCompleted,
    kStopped,
};

// The main thread is the state machine: it owns the DEALER socket and drives
// all transitions. Tasks are executed by a TaskExecutor backed by a single
// common::EventManager worker; the executor signals completion by setting
// kTaskCompleted and queueing the result.
class Client {
public:
    explicit Client(common::Config& config);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Runs INIT then the state machine loop on the calling thread until
    // stop() is called from another thread.
    void start();
    void stop();

private:
    struct LoadedPlugin {
        std::unique_ptr<common::DynamicLibrary> library;
        // Declaration order matters: the plugin instance must be destroyed
        // before the library is unloaded (virtual calls go into the DLL).
        std::unique_ptr<common::WorkerPlugin, ClientPluginDeleter> instance;
    };

    bool LoadPlugins();
    bool RegisterWithServer();  // INIT: synchronous validation, true on success
    bool SendRegisterOnce();
    bool WaitForRegisterResult(common::Envelope& out);
    void RunStateMachine();
    void HandleEnvelope(const common::Envelope& env);
    void HandleTask(const common::TaskCommand& command, const std::string& plugin_type);
    void HandleMessage(const common::Envelope& env);
    void HandleStop();
    void OnTaskCompleted(const common::TaskResult& result);
    void SendHeartbeat();
    void SendRequestTask();
    void SendTaskReport(const common::TaskResult& result);

    common::Config& config_;
    common::ZmqDealer dealer_;
    std::vector<LoadedPlugin> plugins_;
    common::WorkerPlugin* plugin_ = nullptr;
    TaskExecutor executor_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};

    common::BlockingQueue<common::Envelope> outgoing_;

    // Shared with the executor thread.
    mutable std::mutex state_mutex_;
    ClientState state_ = ClientState::kInit;
    common::BlockingQueue<common::TaskResult> completed_results_;

    // State machine timing (main thread only).
    std::chrono::steady_clock::time_point last_heartbeat_;
    std::chrono::steady_clock::time_point next_request_;
    std::chrono::steady_clock::time_point request_sent_at_;
    std::chrono::milliseconds backoff_;

    std::string worker_id_;
    std::string ip_;
    std::chrono::milliseconds heartbeat_interval_;
    std::chrono::milliseconds poll_timeout_;
    std::chrono::milliseconds request_backoff_initial_;
    std::chrono::milliseconds request_backoff_max_;
    std::chrono::milliseconds request_timeout_;
    int register_timeout_ms_ = 0;  // <=0 means registration waits forever
    int register_max_attempts_ = 5;
};

}  // namespace client
