#pragma once

#include "common/WorkerPlugin.h"
#include "stpcompare/StpStore.h"

#include <memory>
#include <string>
#include <vector>

namespace stpcompare {

// Client-side stpcompare plugin: for each stp file in a task, runs the
// configured converter program (which produces info.txt and info_new.txt) and
// writes the two txt outputs into the configured table via the shared store.
// The wire reports only per-file status (the heavy txt stays in the DB).
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
    // Runs the converter once for one stp file and fills the two txt outputs.
    // Returns true on success (both txt produced); on failure fills detail.
    bool RunOneFile(const std::string& run_id, const std::string& file,
                    FileResult* output, const common::CancellationToken& cancellation);

    std::unique_ptr<StpResultStore> store_;
    bool dry_run_ = true;
    std::string stp_root_;
    std::string program_exe_;
    std::string program_cmd_;
    std::vector<std::string> program_args_;
    std::string info_file_;
    std::string info_new_file_;
    std::string work_base_;
    bool keep_work_ = false;
    int dry_run_delay_ms_ = 0;
    int process_timeout_ms_ = 300000;

    // Component versions read from version.txt files in the package.
    std::string program_version_;
    std::string dataset_version_;
};

}  // namespace stpcompare
