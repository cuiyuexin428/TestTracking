#pragma once

#include "common/ClientPlugin.h"
#include "common/Event.h"
#include "common/EventManager.h"
#include "coverage/CoverageStore.h"
#include "coverage.pb.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace coverage_plugin {

// One queued execution action: runs the command (or any plugin work) on the
// plugin's own EventManager pool.
class ExcuteEvent : public common::Event {
public:
    explicit ExcuteEvent(std::function<void()> action);

    void execute() override;

private:
    std::function<void()> action_;
};

// Client-side coverage plugin: executes one task (a list of stp files) with
// OpenCppCoverage over the target program, parses the cobertura report into
// line-level point sets, and writes them into the CoverageStore. Concurrency
// is entirely plugin-owned: it uses its own EventManager pool (task_threads
// from config; 0 means serial execution). execute() blocks until every file
// action finishes, then returns the aggregated result to the framework.
class ClientPlugin : public common::ClientPlugin {
public:
    std::string taskType() const override;
    bool init(const std::string& config) override;
    bool execute(const common::TaskSpec& task, common::TaskResult& result) override;

private:
    void RunFileAction(const coverage::CoverageTask& cov_task, const std::string& file);
    bool RunOneFile(const std::string& run_id, int wave, const std::string& file,
                    std::vector<std::uint64_t>& points);

    std::unique_ptr<CoverageStore> store_;
    std::unique_ptr<common::EventManager> pool_;
    int task_threads_ = 0;
    bool dry_run_ = true;
    std::string stp_root_;
    std::string opencppcoverage_;
    std::string target_exe_;
    std::string export_dir_;
    bool keep_reports_ = false;  // validation only: keep cobertura XMLs
    std::vector<std::string> occt_dlls_;

    // Shared per-task state, protected for concurrent file actions.
    std::mutex mutex_;
    std::condition_variable cv_;
    int pending_ = 0;
    std::vector<coverage::FileCoverage> files_;
};

}  // namespace coverage_plugin
