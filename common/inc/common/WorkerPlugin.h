#pragma once

#include "communication.pb.h"

#include <string>

namespace common {

// Result of processing a registration validation reply. The reply is an
// opaque business message; the plugin decides what to do next.
enum class RegisterOutcome {
    kReady,   // validation passed, enter READY
    kRetry,   // reply handled, re-send REGISTER
    kFailed,  // cannot be satisfied, give up
};

// Client plugin base class. plugin_type anchors the client plugin and the
// server plugin: the client framework routes tasks to the matching plugin.
class WorkerPlugin {
public:
    virtual ~WorkerPlugin() = default;

    // Unique plugin type key, matching the server plugin.
    virtual std::string pluginType() const = 0;

    // Load config and check the local deployment.
    virtual bool init(const std::string& config) = 0;

    // Initialization phase: check the local deployment and produce the
    // opaque registration business message.
    virtual bool collectRegistrationInfo(std::string* payload) = 0;

    // Handle the server plugin reply (e.g. deployment instructions). The
    // reply is opaque; the plugin decides whether the client is ready,
    // needs to re-register, or cannot be satisfied.
    virtual RegisterOutcome handleValidationReply(const std::string& reply_payload) = 0;

    // Execute one task and fill the execution report.
    virtual bool execute(const Task& task, TaskResult& result) = 0;

    // Optional short business info shown in the scheduler's client list
    // (e.g. component versions). Empty string means no extra info.
    virtual std::string heartbeatInfo() const {
        return std::string();
    }
};

// DLL entry points (extern "C", exported):
//   __declspec(dllexport) common::WorkerPlugin* CreateClientPlugin();
//   __declspec(dllexport) void DestroyClientPlugin(common::WorkerPlugin* plugin);
using CreateClientPluginFn = WorkerPlugin* (*)();
using DestroyClientPluginFn = void (*)(WorkerPlugin*);

}  // namespace common
