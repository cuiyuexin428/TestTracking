#pragma once

#include "common/EventManager.h"
#include "common/WorkerPlugin.h"
#include "communication.pb.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace client {

// Executes tasks on a configurable pool of common::EventManager workers.
// The main thread pushes tasks; each worker executes one task and fires the
// completion callback with the result. stop() drops queued tasks, cancels
// completion callbacks, and joins the workers.
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

private:
    common::WorkerPlugin* plugin_ = nullptr;
    std::unique_ptr<common::EventManager> pool_;
    std::mutex mutex_;
    std::atomic<bool> cancelled_{false};
    CompletionCallback on_completed_;
};

}  // namespace client
