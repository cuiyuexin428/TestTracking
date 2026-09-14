#pragma once

#include "common/EventManager.h"
#include "common/CancellationToken.h"
#include "common/WorkerPlugin.h"
#include "communication.pb.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace client {

// Executes tasks on a configurable pool of common::EventManager workers.
// The main thread pushes tasks; each worker executes one task and fires the
// completion callback with the result. The owner starts, pushes and joins;
// requestStop is safe to call concurrently and never waits for execution.
class TaskExecutor {
public:
    using CompletionCallback = std::function<void(const common::TaskResult&)>;

    TaskExecutor() = default;
    ~TaskExecutor();

    TaskExecutor(const TaskExecutor&) = delete;
    TaskExecutor& operator=(const TaskExecutor&) = delete;

    void start(common::WorkerPlugin* plugin, CompletionCallback on_completed, int thread_count);
    void push(const common::Task& task);
    void stop();
    // Requests cancellation and drops queued work; the communication loop
    // polls isStopped before calling stop/join at a safe lifecycle boundary.
    void requestStop();
    bool isStopped() const;

private:
    common::WorkerPlugin* plugin_ = nullptr;
    std::unique_ptr<common::EventManager> pool_;
    mutable std::mutex mutex_;
    common::CancellationSource cancellation_source_;
    CompletionCallback on_completed_;
};

}  // namespace client
