// Minimal concrete plugin used to exercise the framework end to end:
// scheduler side plans/dispatches tasks, client side executes them.
#include <chrono>
#include <deque>
#include <json.hpp>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/Logger.h"
#include "common/TaskPlugin.h"
#include "common/WorkerPlugin.h"
#include "demo.pb.h"

#ifdef _WIN32
#define DEMO_EXPORT __declspec(dllexport)
#else
#define DEMO_EXPORT __attribute__((visibility("default")))
#endif

namespace {

class DemoPlugin : public common::TaskPlugin {
   public:
    std::string pluginType() const override { return "demo"; }

    bool init(const std::string& config) override {
        (void)config;
        return true;
    }

    bool validateClient(const common::ClientInfo& client, const std::string& client_message,
                        common::ValidationResult& result) override {
        (void)client;
        (void)client_message;
        result.accepted = true;
        result.reason = "demo accepts all";
        return true;
    }

    bool plan(const common::RunRequest& request,
              const std::vector<common::ClientInfo>& clients) override {
        (void)clients;
        const int total = 10;

        std::lock_guard<std::mutex> lock(mutex_);
        pending_.clear();
        pending_ids_.clear();
        tasks_by_id_.clear();
        task_client_.clear();
        done_.clear();
        completed_ = 0;
        total_ = total;

        for (int i = 0; i < total_; ++i) {
            demo::DemoTask task;
            task.set_task_index(i);
            task.set_description("demo task " + std::to_string(i));

            common::Task spec;
            spec.set_task_id(request.run_id + "-" + std::to_string(i));
            task.SerializeToString(spec.mutable_payload());

            tasks_by_id_[spec.task_id()] = spec;
            pending_ids_.insert(spec.task_id());
            pending_.push_back(std::move(spec));
        }
        LOG_INFO("demo plugin: planned " << total_ << " tasks for run " << request.run_id);
        return true;
    }

    bool requestTask(const common::ClientInfo& client, const std::string& request_payload,
                     common::Task& task) override {
        (void)request_payload;
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_.empty()) {
            return false;
        }
        task = pending_.front();
        pending_.pop_front();
        pending_ids_.erase(task.task_id());
        task_client_[task.task_id()] = client.identity;
        return true;
    }

    void requeue(const std::vector<std::string>& task_ids) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const std::string& task_id : task_ids) {
            if (done_.count(task_id)) continue;
            task_client_.erase(task_id);
            const auto spec_it = tasks_by_id_.find(task_id);
            if (spec_it != tasks_by_id_.end()) {
                if (pending_ids_.insert(task_id).second) {
                    pending_.push_back(spec_it->second);
                } else {
                    LOG_INFO("demo plugin: task already pending, skip requeue " << task_id);
                }
            }
            LOG_INFO("demo plugin: requeued " << task_id);
        }
    }

    bool saveCheckpoint(const std::string& run_id) override {
        (void)run_id;
        return true;  // demo has no persistent state
    }

    bool restore(const std::string& run_id) override {
        (void)run_id;
        return false;  // demo runs cannot be resumed
    }

    bool processResult(const common::TaskResult& result) override {
        demo::DemoResult demo_result;
        std::string message = "unparsed";
        if (demo_result.ParseFromString(result.result())) {
            message = demo_result.message();
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (!tasks_by_id_.count(result.task_id())) return false;
        if (!done_.insert(result.task_id()).second) return true;
        task_client_.erase(result.task_id());
        ++completed_;
        LOG_INFO("demo plugin: result " << result.task_id() << " exit=" << result.exit_code()
                                        << " msg=" << message << " (" << completed_ << "/" << total_
                                        << ")");
        if (total_ > 0 && completed_ >= total_) {
            LOG_INFO("demo plugin: run finished");
        }
        return true;
    }

    bool runFinished() override {
        std::lock_guard<std::mutex> lock(mutex_);
        return total_ > 0 && completed_ >= total_ && pending_.empty();
    }

    void cancel() override {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.clear();
        pending_ids_.clear();
        tasks_by_id_.clear();
        task_client_.clear();
        done_.clear();
        completed_ = 0;
        total_ = 0;
        LOG_INFO("demo plugin: run cancelled, task pool cleared");
    }

   private:
    std::mutex mutex_;
    std::deque<common::Task> pending_;
    std::unordered_set<std::string> done_;
    std::unordered_set<std::string> pending_ids_;
    std::unordered_map<std::string, common::Task> tasks_by_id_;
    std::unordered_map<std::string, std::string> task_client_;
    int completed_ = 0;
    int total_ = 0;
};

class DemoClientPlugin : public common::WorkerPlugin {
   public:
    std::string pluginType() const override { return "demo"; }

    bool init(const std::string& config) override {
        const nlohmann::json cfg = nlohmann::json::parse(config);
        sleep_ms_ = cfg.value("task_sleep_ms", 200);
        return true;
    }

    bool collectRegistrationInfo(std::string* payload) override {
        payload->clear();
        return true;
    }

    common::RegisterOutcome handleValidationReply(const std::string& reply_payload) override {
        (void)reply_payload;
        return common::RegisterOutcome::kReady;
    }

    bool execute(const common::Task& task, common::TaskResult& result) override {
        return execute(task, result, {});
    }

    bool execute(const common::Task& task, common::TaskResult& result,
                 const common::CancellationToken& cancellation) override {
        demo::DemoTask demo_task;
        if (!demo_task.ParseFromString(task.payload())) {
            result.set_exit_code(-1);
            return false;
        }

        const auto start = std::chrono::steady_clock::now();
        if (cancellation.waitFor(std::chrono::milliseconds(sleep_ms_))) {
            result.set_exit_code(-2);
            return false;
        }
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - start)
                                    .count();

        result.set_exit_code(0);
        result.set_duration_ms(elapsed_ms);
        result.set_artifact_path("artifacts/" + task.task_id() + ".txt");

        demo::DemoResult demo_result;
        demo_result.set_ok(true);
        demo_result.set_message("demo done: " + demo_task.description());
        demo_result.SerializeToString(result.mutable_result());
        return true;
    }

   private:
    int sleep_ms_ = 200;
};

}  // namespace

extern "C" DEMO_EXPORT common::TaskPlugin* CreatePlugin() {
    return new DemoPlugin;
}

extern "C" DEMO_EXPORT void DestroyPlugin(common::TaskPlugin* plugin) {
    delete plugin;
}

extern "C" DEMO_EXPORT common::WorkerPlugin* CreateClientPlugin() {
    return new DemoClientPlugin;
}

extern "C" DEMO_EXPORT void DestroyClientPlugin(common::WorkerPlugin* plugin) {
    delete plugin;
}
