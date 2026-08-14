#pragma once

#include "communication.pb.h"

#include <string>

namespace common {

// Abstract base class for client-side task executors. One DLL per task type,
// loaded by the client framework. The framework only depends on this
// interface; concrete executors are separate DLLs.
class ClientPlugin {
public:
    virtual ~ClientPlugin() = default;

    // Unique task type key, e.g. "stp"; must match RunWaveCommand.task_type.
    virtual std::string taskType() const = 0;

    // Called once after loading; config is this plugin's JSON section text.
    virtual bool init(const std::string& config) = 0;

    // Execute one task. The plugin parses task.payload() (its own business
    // proto), runs the test program, and fills result fields (exit_code,
    // duration_ms, artifact_path, result bytes).
    virtual bool execute(const TaskSpec& task, TaskResult& result) = 0;
};

// DLL entry points (extern "C", exported):
//   __declspec(dllexport) common::ClientPlugin* CreateClientPlugin();
//   __declspec(dllexport) void DestroyClientPlugin(common::ClientPlugin* plugin);
using CreateClientPluginFn = ClientPlugin* (*)();
using DestroyClientPluginFn = void (*)(ClientPlugin*);

}  // namespace common
