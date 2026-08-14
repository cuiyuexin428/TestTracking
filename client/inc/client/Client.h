#pragma once

#include "common/BlockingQueue.h"
#include "common/ClientPlugin.h"
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

// Deletes a client plugin instance through the DestroyClientPlugin factory
// exported by its DLL.
struct ClientPluginDeleter {
    common::DestroyClientPluginFn destroy = nullptr;

    void operator()(common::ClientPlugin* plugin) const {
        if (plugin != nullptr && destroy != nullptr) {
            destroy(plugin);
        }
    }
};

// Client framework: one DEALER socket owned by a poll thread (receives
// commands, sends heartbeats/results), one executor thread that runs tasks
// through the configured client plugin. Framework code depends only on the
// abstract common::ClientPlugin interface.
class Client {
public:
    explicit Client(common::Config& config);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void start();
    void stop();

private:
    struct LoadedPlugin {
        std::unique_ptr<common::DynamicLibrary> library;
        // Declaration order matters: the plugin instance must be destroyed
        // before the library is unloaded (virtual calls go into the DLL).
        std::unique_ptr<common::ClientPlugin, ClientPluginDeleter> instance;
    };

    bool LoadPlugins();
    void RunPoll();
    void RunExecutor();
    void HandleMessage(const common::Envelope& env);
    void HandleRunWave(const common::RunWaveCommand& command);
    void SendHeartbeat();
    void SendResponse(const common::TaskResult& result);

    common::Config& config_;
    common::ZmqDealer dealer_;
    std::vector<LoadedPlugin> plugins_;
    common::ClientPlugin* plugin_ = nullptr;

    std::atomic<bool> running_{false};
    std::thread poll_thread_;
    std::thread executor_thread_;

    common::BlockingQueue<common::TaskSpec> tasks_;
    common::BlockingQueue<common::Envelope> outgoing_;

    std::mutex state_mutex_;
    bool busy_ = false;
    std::string current_task_id_;

    std::string worker_id_;
    std::string program_version_;
    std::string dataset_version_;
    bool dataset_ready_ = false;
    std::chrono::milliseconds heartbeat_interval_;
    std::chrono::milliseconds poll_timeout_;
};

}  // namespace client
