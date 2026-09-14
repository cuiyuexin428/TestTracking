#pragma once

#include "communication.pb.h"
#include "heartbeat.pb.h"

#include <string>
#include <vector>

namespace common {

// Client information: the framework-managed ROUTER identity plus the
// client's plugin type and self-reported heartbeat.
struct ClientInfo {
    std::string identity;
    std::string plugin_type;
    Heartbeat heartbeat;
    std::int64_t last_seen_age_ms = 0;  // filled by the scheduler snapshot
    bool registered = false;            // filled by the scheduler snapshot
    std::string peer_address;           // server-observed ip:port of the client
};

// Run request.
struct RunRequest {
    std::string run_id;
    std::string dataset_version;
    std::string params;
};

// Registration validation result. reply_payload is the opaque business
// reply, which the framework does not parse.
struct ValidationResult {
    bool accepted = false;
    std::string reply_payload;
    std::string reason;
};

// Server plugin base class. plugin_type anchors the server plugin and the
// client plugin: the framework routes REGISTER / REQUEST_TASK / TASK_REPORT
// by plugin_type and passes business payloads through untouched.
class TaskPlugin {
public:
    virtual ~TaskPlugin() = default;

    // Unique plugin type key, matching the client plugin.
    virtual std::string pluginType() const = 0;

    // Load config and check business prerequisites (program/data/manifest/
    // shared directory, etc.).
    virtual bool init(const std::string& config) = 0;

    // Initialization-phase validation, called on REGISTER.
    virtual bool validateClient(const ClientInfo& client,
                                const std::string& client_message,
                                ValidationResult& result) = 0;

    // Persist the run plan before returning true. Business payloads remain
    // plugin-owned; the shared ledger manages pending and committed tasks.
    virtual bool plan(const RunRequest& request,
                      const std::vector<ClientInfo>& clients) = 0;

    // Called on client REQUEST_TASK: fill task and return true when a task
    // is available, false otherwise. request_payload is opaque business data
    // carried by the client request.
    virtual bool requestTask(const ClientInfo& client,
                             const std::string& request_payload,
                             Task& task) = 0;

    // All runtime calls are serialized on the scheduler worker, including
    // STOP/checkpoint/cancel. No callback survives its run's lifecycle barrier.
    // Return true only after durable progress acceptance. Retrying the same
    // run/task must be idempotent; false keeps the framework attempt in flight.
    virtual bool processResult(const TaskResult& result) = 0;

    // Whether the current run has fully completed (no pending work). Called
    // by the framework to clear the active run context so a new run can
    // start; business semantics are decided by the plugin.
    virtual bool runFinished() = 0;

    // Cancel the current run: the plugin discards its task pool (including
    // un-dispatched tasks). Called by the framework when a run is stopped.
    virtual void cancel() = 0;

    // Return in-flight tasks of offline clients to the plugin.
    virtual void requeue(const std::vector<std::string>& task_ids) = 0;

    // Persist the current run state so it can be resumed after a stop or
    // crash. Called by the framework before cancel() on STOP; plugins may
    // also save continuously. Return false if nothing could be saved.
    virtual bool saveCheckpoint(const std::string& run_id) = 0;

    // Rebuild the run state from a previously saved checkpoint. Called by
    // the framework at scheduler startup for unfinished runs. Return false
    // when the run cannot be resumed.
    virtual bool restore(const std::string& run_id) = 0;
};

// DLL entry points (extern "C", exported):
//   __declspec(dllexport) common::TaskPlugin* CreatePlugin();
//   __declspec(dllexport) void DestroyPlugin(common::TaskPlugin* plugin);
using CreatePluginFn = TaskPlugin* (*)();
using DestroyPluginFn = void (*)(TaskPlugin*);

}  // namespace common
