// Minimal concrete plugin used to exercise the framework end to end:
// scheduler side plans/dispatches tasks, client side executes them.
#include "common/ClientPlugin.h"
#include "common/Logger.h"
#include "common/Plugin.h"

#include "demo.pb.h"

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#define DEMO_EXPORT __declspec(dllexport)
#else
#define DEMO_EXPORT __attribute__((visibility("default")))
#endif

namespace {

class DemoPlugin : public common::Plugin {
public:
    std::string taskType() const override {
        return "demo";
    }

    bool init(const std::string& config) override {
        (void)config;
        return true;
    }

    bool plan(const common::RunRequest& request,
              const std::vector<common::ClientInfo>& clients) override {
        (void)clients;
        const int total = 10;

        std::lock_guard<std::mutex> lock(mutex_);
        pending_.clear();
        tasks_by_id_.clear();
        task_client_.clear();
        client_tasks_.clear();
        completed_ = 0;
        total_ = total;

        for (int i = 0; i < total_; ++i) {
            demo::DemoTask task;
            task.set_task_index(i);
            task.set_description("demo task " + std::to_string(i));

            common::TaskSpec spec;
            spec.set_task_id(request.run_id + "-" + std::to_string(i));
            task.SerializeToString(spec.mutable_payload());

            tasks_by_id_[spec.task_id()] = spec;
            pending_.push_back(std::move(spec));
        }
        LOG_INFO("demo plugin: planned " << total_ << " tasks for run " << request.run_id);
        return true;
    }

    bool dispatch(const std::vector<common::ClientInfo>& clients,
                  std::vector<common::TaskAssignment>& out) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const common::ClientInfo& client : clients) {
            if (pending_.empty()) {
                break;
            }
            if (client.heartbeat.status() != common::Heartbeat::IDLE) {
                continue;
            }
            if (client_tasks_[client.identity] > 0) {
                continue;  // one task at a time per client
            }

            common::TaskAssignment assignment;
            assignment.client_identity = client.identity;
            assignment.task = pending_.front();
            pending_.erase(pending_.begin());
            task_client_[assignment.task.task_id()] = client.identity;
            ++client_tasks_[client.identity];
            out.push_back(std::move(assignment));
        }
        return true;
    }

    void requeue(const std::vector<std::string>& task_ids) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const std::string& task_id : task_ids) {
            const auto it = task_client_.find(task_id);
            if (it != task_client_.end()) {
                --client_tasks_[it->second];
                task_client_.erase(it);
            }
            const auto spec_it = tasks_by_id_.find(task_id);
            if (spec_it != tasks_by_id_.end()) {
                pending_.push_back(spec_it->second);
            }
            LOG_INFO("demo plugin: requeued " << task_id);
        }
    }

    void processResult(const common::TaskResult& result) override {
        demo::DemoResult demo_result;
        std::string message = "unparsed";
        if (demo_result.ParseFromString(result.result())) {
            message = demo_result.message();
        }

        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = task_client_.find(result.task_id());
        if (it != task_client_.end()) {
            --client_tasks_[it->second];
            task_client_.erase(it);
        }
        ++completed_;
        LOG_INFO("demo plugin: result " << result.task_id() << " exit=" << result.exit_code()
                                        << " msg=" << message << " (" << completed_ << "/"
                                        << total_ << ")");
        if (total_ > 0 && completed_ >= total_) {
            LOG_INFO("demo plugin: run finished");
        }
    }

private:
    std::mutex mutex_;
    std::vector<common::TaskSpec> pending_;
    std::unordered_map<std::string, common::TaskSpec> tasks_by_id_;
    std::unordered_map<std::string, std::string> task_client_;
    std::unordered_map<std::string, int> client_tasks_;
    int completed_ = 0;
    int total_ = 0;
};

class DemoClientPlugin : public common::ClientPlugin {
public:
    std::string taskType() const override {
        return "demo";
    }

    bool init(const std::string& config) override {
        (void)config;
        return true;
    }

    bool execute(const common::TaskSpec& task, common::TaskResult& result) override {
        demo::DemoTask demo_task;
        if (!demo_task.ParseFromString(task.payload())) {
            result.set_exit_code(-1);
            return false;
        }

        const auto start = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                  start)
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
};

}  // namespace

extern "C" DEMO_EXPORT common::Plugin* CreatePlugin() {
    return new DemoPlugin;
}

extern "C" DEMO_EXPORT void DestroyPlugin(common::Plugin* plugin) {
    delete plugin;
}

extern "C" DEMO_EXPORT common::ClientPlugin* CreateClientPlugin() {
    return new DemoClientPlugin;
}

extern "C" DEMO_EXPORT void DestroyClientPlugin(common::ClientPlugin* plugin) {
    delete plugin;
}
