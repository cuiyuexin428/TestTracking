#pragma once

#include "common/WorkerPlugin.h"
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
class ClientPlugin : public common::WorkerPlugin {
public:
    std::string pluginType() const override;
    bool init(const std::string& config) override;
    bool collectRegistrationInfo(std::string* payload) override;
    common::RegisterOutcome handleValidationReply(const std::string& reply_payload) override;
    bool execute(const common::Task& task, common::TaskResult& result) override;

private:
    void RunFileAction(const coverage::CoverageTask& cov_task, const std::string& file);
    bool RunOneFile(const std::string& run_id, int wave, const std::string& file,
                    std::vector<std::uint64_t>& points);

    std::unique_ptr<CoverageStore> store_;
    std::unique_ptr<common::EventManager> pool_;
    int task_threads_ = 0;
    bool dry_run_ = true;
    std::string stp_root_;
    std::string program_dir_;  // local deployment dir for the test program
    std::string opencppcoverage_;
    std::string target_exe_;
    std::string export_dir_;
    bool keep_reports_ = false;  // validation only: keep cobertura XMLs
    bool store_cov_ = false;     // keep OpenCppCoverage .cov binaries in the store
    std::vector<std::string> occt_dlls_;

    // Local deployment state (registration business message content).
    std::string program_version_;
    std::string dataset_version_;
    bool deployed_ = false;

    // Shared per-task state, protected for concurrent file actions.
    std::mutex mutex_;
    std::condition_variable cv_;
    int pending_ = 0;
    std::vector<coverage::FileCoverage> files_;
};

}  // namespace coverage_plugin
