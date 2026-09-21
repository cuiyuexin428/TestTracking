#pragma once

#include "common/WorkerPlugin.h"
#include "stpcompare/StpStore.h"

#include <filesystem>
#include <json.hpp>
#include <memory>
#include <string>
#include <vector>

namespace stpcompare {

// Client-side stpcompare plugin: for each stp file in a task, runs the
// configured baseline and candidate programs. Each program writes info.txt and
// info_new.txt into its own output directory. The four opaque outputs are
// published atomically; comparison belongs to later database-only analysis.
class ClientPlugin : public common::WorkerPlugin {
public:
    std::string pluginType() const override;
    bool init(const std::string& config) override;
    bool collectRegistrationInfo(std::string* payload) override;
    common::RegisterOutcome handleValidationReply(const std::string& reply_payload) override;
    std::string heartbeatInfo() const override;
    bool execute(const common::Task& task, common::TaskResult& result) override;
    bool execute(const common::Task& task, common::TaskResult& result,
                 const common::CancellationToken& cancellation) override;

private:
    struct Program {
        std::string role;
        std::string executable;
        std::vector<std::string> arguments;
        std::string version;
    };
    bool ReadProgram(const nlohmann::json& config,const std::string& role,Program* program);
    bool RunProgram(const Program& program,const std::filesystem::path& task_dir,
                    const std::string& stp_path,const std::string& file,
                    const std::string& run_id,FileResult* output,
                    const common::CancellationToken& cancellation);
    bool RunOneFile(const std::string& run_id, const std::string& file,
                    FileResult* output, const common::CancellationToken& cancellation);

    std::unique_ptr<StpResultStore> store_;
    bool dry_run_ = true;
    std::string stp_root_;
    Program baseline_;
    Program candidate_;
    std::string work_base_;
    bool keep_work_ = false;
    int dry_run_delay_ms_ = 0;
    int process_timeout_ms_ = 300000;

    std::string dataset_version_;
};

}  // namespace stpcompare
