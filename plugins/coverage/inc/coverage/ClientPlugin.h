#pragma once

#include "common/ClientPlugin.h"
#include "coverage/CoverageStore.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace coverage_plugin {

// Client-side coverage plugin: executes one task (a list of stp files) with
// OpenCppCoverage over the target program, parses the cobertura report into
// line-level point sets, and writes them into the CoverageStore. dry_run mode
// produces synthetic points so the pipeline runs without the real tools.
class ClientPlugin : public common::ClientPlugin {
public:
    std::string taskType() const override;
    bool init(const std::string& config) override;
    bool execute(const common::TaskSpec& task, common::TaskResult& result) override;

private:
    bool RunOneFile(const std::string& run_id, int wave, const std::string& file,
                    std::vector<std::uint64_t>& points);

    std::unique_ptr<CoverageStore> store_;
    bool dry_run_ = true;
    std::string stp_root_;
    std::string opencppcoverage_;
    std::string target_exe_;
    std::string export_dir_;
    bool keep_reports_ = false;  // validation only: keep cobertura XMLs
    std::vector<std::string> occt_dlls_;
};

}  // namespace coverage_plugin
