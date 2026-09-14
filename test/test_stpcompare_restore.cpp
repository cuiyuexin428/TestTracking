// Regression test: after STOP/crash, tasks that were in flight at checkpoint
// time must be re-dispatched by restore(), not silently treated as done.
//
// Drives the real stpcompare SchedulerPlugin against the file-backed store:
//   1. plan a run from a temp manifest;
//   2. requestTask twice so two tasks are in flight;
//   3. saveCheckpoint (snapshots pending + in-flight);
//   4. restore into a fresh plugin instance;
//   5. assert restore re-dispatches BOTH the in-flight and the still-pending
//      tasks (so no task is lost), and the run is not finished.
#include "stpcompare/SchedulerPlugin.h"

#include "common/Logger.h"

#include <json.hpp>

#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                             \
    do {                                                             \
        if (!(cond)) {                                               \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                            \
        }                                                            \
    } while (0)

bool Contains(const std::vector<std::string>& v, const std::string& s) {
    for (const std::string& x : v) {
        if (x == s) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("stpcompare-test-"+
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    std::filesystem::create_directories(root / "store", ec);

    const std::filesystem::path manifest = root / "manifest.txt";
    {
        std::ofstream out(manifest.string());
        out << "a.stp,1\nb.stp,1\nc.stp,1\nd.stp,1\ne.stp,1\n";
    }

    const nlohmann::json cfg = {
        {"stp_manifest", manifest.string()},
        {"store_type", "file"},
        {"store_dir", (root / "store").string()},
        {"task_size", 2},
    };
    const std::string config = cfg.dump();

    // First instance: plan and put two tasks in flight.
    {
        stpcompare::SchedulerPlugin p1;
        CHECK(p1.init(config), "p1 init");
        common::RunRequest req;
        req.run_id = "run-1";
        CHECK(p1.plan(req, {}), "p1 plan");

        common::ClientInfo client;
        client.identity = "worker-1";
        std::vector<std::string> in_flight;
        for (int i = 0; i < 2; ++i) {
            common::Task task;
            if (p1.requestTask(client, "", task)) {
                in_flight.push_back(task.task_id());
            }
        }
        CHECK(in_flight.size() == 2, "two tasks dispatched (in flight)");
        CHECK(!p1.runFinished(), "run not finished while tasks in flight");
        CHECK(p1.saveCheckpoint("run-1"), "p1 saveCheckpoint");

        // Second instance: restore must re-dispatch the same tasks.
        stpcompare::SchedulerPlugin p2;
        CHECK(p2.init(config), "p2 init");
        CHECK(p2.restore("run-1"), "p2 restore");

        std::vector<std::string> re_dispatched;
        while (true) {
            common::Task task;
            if (!p2.requestTask(client, "", task)) {
                break;
            }
            re_dispatched.push_back(task.task_id());
        }
        // The checkpoint had 2 in-flight + 1 pending = 3 unfinished tasks; all
        // must be re-dispatched (in-flight no longer silently lost).
        CHECK(re_dispatched.size() == 3, "all 3 unfinished tasks re-dispatched");
        for (const std::string& id : in_flight) {
            CHECK(Contains(re_dispatched, id),
                  ("in-flight task survived restore: " + id).c_str());
        }
        CHECK(!p2.runFinished(), "restored run not finished");
    }

    std::filesystem::remove_all(root, ec);
    if (g_failures == 0) {
        std::printf("test_stpcompare_restore: OK\n");
        return 0;
    }
    std::printf("test_stpcompare_restore: %d failures\n", g_failures);
    return 1;
}
