#pragma once

#include "common/EventManager.h"
#include "common/WorkerPlugin.h"
#include "communication.pb.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace client {

// Executes tasks serially on a single common::EventManager worker thread.
// The main thread pushes tasks; on completion the callback fires with the
// result. stop() drops queued tasks, cancels completion callbacks, and
// joins the worker.
class TaskExecutor {
public:
    using CompletionCallback = std::function<void(const common::TaskResult&)>;

    TaskExecutor() = default;
    ~TaskExecutor();

    TaskExecutor(const TaskExecutor&) = delete;
    TaskExecutor& operator=(const TaskExecutor&) = delete;

    void start(common::WorkerPlugin* plugin, CompletionCallback on_completed);
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
