#include "client/TaskExecutor.h"

#include "common/Logger.h"

#include <stdexcept>
#include <utility>

namespace client {

namespace {

class ExecuteTaskEvent : public common::Event {
public:
    explicit ExecuteTaskEvent(std::function<void()> action) : action_(std::move(action)) {
    }

    void execute() override {
        action_();
    }

private:
    std::function<void()> action_;
};

}  // namespace

TaskExecutor::~TaskExecutor() {
    stop();
}

void TaskExecutor::start(common::WorkerPlugin* plugin, CompletionCallback on_completed,
                         int thread_count) {
    if (plugin == nullptr) {
        throw std::invalid_argument("TaskExecutor: null plugin");
    }
    if (thread_count < 1) {
        throw std::invalid_argument("TaskExecutor: thread_count must be >= 1");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (pool_ != nullptr) {
        throw std::logic_error("TaskExecutor: already started");
    }
    plugin_ = plugin;
    on_completed_ = std::move(on_completed);
    cancelled_.store(false);
    pool_ = std::make_unique<common::EventManager>(thread_count);
}

void TaskExecutor::push(const common::Task& task) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pool_ == nullptr) {
        LOG_ERROR("TaskExecutor: not started, dropping task " << task.task_id());
        return;
    }
    common::WorkerPlugin* plugin = plugin_;
    CompletionCallback on_completed = on_completed_;
    std::atomic<bool>& cancelled = cancelled_;

    auto action = [plugin, on_completed, &cancelled, task] {
        common::TaskResult result;
        result.set_task_id(task.task_id());
        bool ok = false;
        try {
            ok = plugin->execute(task, result);
        } catch (const std::exception& e) {
            LOG_ERROR("client plugin " << plugin->pluginType()
                                       << ": execute threw: " << e.what());
        } catch (...) {
            LOG_ERROR("client plugin " << plugin->pluginType()
                                      << ": execute threw unknown exception");
        }
        if (!ok) {
            result.set_exit_code(-1);
        }
        if (!cancelled.load(std::memory_order_relaxed) && on_completed) {
            on_completed(result);
        }
    };
    pool_->push(new ExecuteTaskEvent(std::move(action)));
}

void TaskExecutor::stop() {
    cancelled_.store(true, std::memory_order_relaxed);
    std::unique_ptr<common::EventManager> pool;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pool = std::move(pool_);
    }
    // The EventManager destructor joins its worker; queued events are dropped.
    pool.reset();
}

}  // namespace client
