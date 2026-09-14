#pragma once
#include "common/TaskPlugin.h"
#include "common/TaskLedger.h"
#include <string>
namespace coverage_plugin {
// Business payload adapter. Lifecycle and durable progress are shared.
class SchedulerPlugin : public common::TaskPlugin {
public:
    std::string pluginType() const override;
    bool init(const std::string& config) override;
    bool validateClient(const common::ClientInfo&,const std::string&,common::ValidationResult&) override;
    bool plan(const common::RunRequest&,const std::vector<common::ClientInfo>&) override;
    bool requestTask(const common::ClientInfo&,const std::string&,common::Task&) override;
    bool processResult(const common::TaskResult&) override;
    bool runFinished() override;
    void cancel() override;
    void requeue(const std::vector<std::string>&) override;
    bool saveCheckpoint(const std::string&) override;
    bool restore(const std::string&) override;
private:
    common::TaskLedger ledger_;
    std::string manifest_path_,expected_program_version_,expected_dataset_version_;
    int task_size_=25;
};
}
