#pragma once

#include "communication.pb.h"
#include "heartbeat.pb.h"

#include <string>
#include <vector>

namespace common {

// A client as seen by the plugin: the framework-managed ROUTER identity plus
// the client's self-reported heartbeat.
struct ClientInfo {
    std::string identity;
    Heartbeat heartbeat;
};

// A run request submitted by the user.
struct RunRequest {
    std::string run_id;
    std::string dataset_version;
    std::string params;  // plugin-specific run parameters
};

// A task assigned to a specific client by the plugin.
struct TaskAssignment {
    std::string client_identity;
    TaskSpec task;  // task_id + opaque business payload
};

// Abstract base class for task plugins. The scheduler-side plugin owns its
// task pool and decides which client gets which task; the framework triggers
// dispatch, transports assignments, tracks in-flight tasks, and hands back
// tasks of offline clients via requeue().
class Plugin {
public:
    virtual ~Plugin() = default;

    // Unique task type key, e.g. "stp"; must match RunWaveCommand.task_type.
    virtual std::string taskType() const = 0;

    // Called once after loading; config is this plugin's JSON section text.
    virtual bool init(const std::string& config) = 0;

    // Plan a run. The plugin keeps the generated task pool internally.
    // clients is a snapshot; no tasks are sent here.
    virtual bool plan(const RunRequest& request,
                      const std::vector<ClientInfo>& clients) = 0;

    // Called when there may be work to send (run init, client list change,
    // completion, requeue). The plugin picks eligible idle clients from the
    // snapshot and fills out. May return empty when nothing can be sent.
    virtual bool dispatch(const std::vector<ClientInfo>& clients,
                          std::vector<TaskAssignment>& out) = 0;

    // Tasks that were in flight to a now-offline client are returned here so
    // the plugin can put them back into its pool.
    virtual void requeue(const std::vector<std::string>& task_ids) = 0;

    // Process one finished task (parse business result, persist, ...).
    virtual void processResult(const TaskResult& result) = 0;
};

// DLL entry points (extern "C", exported):
//   __declspec(dllexport) common::Plugin* CreatePlugin();
//   __declspec(dllexport) void DestroyPlugin(common::Plugin* plugin);
using CreatePluginFn = Plugin* (*)();
using DestroyPluginFn = void (*)(Plugin*);

}  // namespace common
