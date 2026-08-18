#pragma once

#include "common/WorkerPlugin.h"
#include "coverage/CoverageStore.h"
#include "coverage.pb.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace coverage_plugin {

// Client-side coverage plugin: executes one task (a list of stp files) with
// OpenCppCoverage over the target program, parses the cobertura report into
// line-level point sets, and writes them into the CoverageStore. Concurrency
// is owned by the client framework: one framework worker executes one task,
// and the files inside a task run sequentially.
class ClientPlugin : public common::WorkerPlugin {
public:
    std::string pluginType() const override;
    bool init(const std::string& config) override;
    bool collectRegistrationInfo(std::string* payload) override;
    common::RegisterOutcome handleValidationReply(const std::string& reply_payload) override;
    bool execute(const common::Task& task, common::TaskResult& result) override;

private:
    bool RunOneFile(const std::string& run_id, int wave, const std::string& file,
                    std::vector<std::uint64_t>& points);

    std::unique_ptr<CoverageStore> store_;
    bool dry_run_ = true;
    std::string stp_root_;
    std::string program_dir_;  // testenv/program in the client package
    std::string opencppcoverage_;
    std::string target_exe_;
    std::string export_dir_;
    bool keep_reports_ = false;  // validation only: keep cobertura XMLs
    bool store_cov_ = false;     // keep OpenCppCoverage .cov binaries in the store
    std::vector<std::string> occt_dlls_;

    // Component versions read from version.txt files in the package.
    std::string program_version_;
    std::string dataset_version_;
};

}  // namespace coverage_plugin
