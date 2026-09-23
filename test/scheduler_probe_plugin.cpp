#include "common/TaskPlugin.h"
#include <json.hpp>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace {
class Probe : public common::TaskPlugin {
public:
    std::string pluginType() const override { return type_; }
    bool init(const std::string& config) override {
        const auto settings = nlohmann::json::parse(config);
        entered_ = settings.at("entered_file").get<std::string>();
        type_ = settings.value("plugin_type", std::string("scheduler-probe"));
        throw_plan_ = settings.value("throw_plan", false);
        if (settings.value("expected_program_version", std::string()) == "reject-init") return false;
        return true;
    }
    bool validateClient(const common::ClientInfo&, const std::string&, common::ValidationResult& reply) override {
        reply.accepted = true;
        return true;
    }
    bool plan(const common::RunRequest& run, const std::vector<common::ClientInfo>&) override {
        if (throw_plan_) throw std::runtime_error("requested plan failure");
        task_.Clear();
        task_.set_task_id(run.run_id + "-task");
        task_.set_run_id(run.run_id);
        pending_ = true;
        done_ = false;
        return true;
    }
    bool requestTask(const common::ClientInfo&, const std::string&, common::Task& task) override {
        if (!pending_) return false;
        pending_ = false;
        task = task_;
        return true;
    }
    bool processResult(const common::TaskResult& result) override {
        if (result.task_id() != task_.task_id()) return false;
        if (done_) return true;
        std::ofstream(entered_) << "entered";
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        done_ = true;
        return true;
    }
    bool runFinished() override { return done_; }
    void cancel() override { pending_ = false; done_ = false; }
    void requeue(const std::vector<std::string>& tasks) override {
        for (const auto& task : tasks) if (task == task_.task_id() && !done_) pending_ = true;
    }
    bool saveCheckpoint(const std::string&) override { return true; }
    bool restore(const std::string&) override { return false; }
private:
    common::Task task_;
    std::string entered_;
    std::string type_ = "scheduler-probe";
    bool throw_plan_ = false;
    bool pending_ = false, done_ = false;
};
}
#ifdef _WIN32
#define TEST_EXPORT __declspec(dllexport)
#else
#define TEST_EXPORT __attribute__((visibility("default")))
#endif
extern "C" TEST_EXPORT common::TaskPlugin* CreatePlugin() { return new Probe; }
extern "C" TEST_EXPORT void DestroyPlugin(common::TaskPlugin* plugin) { delete plugin; }
