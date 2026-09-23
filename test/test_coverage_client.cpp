// Exercises the real client and file store without a business database or
// OpenCppCoverage installation. This executable also serves as a fake coverage
// process so the test observes the actual command-line argument boundaries.
#include "coverage/ClientPlugin.h"
#include "coverage/CoverageStore.h"
#include "common/Util.h"

#include <json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

int failures = 0;

#define CHECK(condition, message)                                             \
    do {                                                                      \
        if (!(condition)) {                                                   \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", message, __FILE__, __LINE__); \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

void Write(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

int FakeCoverage(int argc, char** argv) {
    std::string xml;
    std::string cov;
    bool module_ok = false;
    int separator = 0;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--") {
            separator = i;
            break;
        }
        const std::string xml_prefix = "--export_type=cobertura:";
        const std::string cov_prefix = "--export_type=binary:";
        const std::string module_prefix = "--modules=";
        if (arg.rfind(xml_prefix, 0) == 0) {
            xml = arg.substr(xml_prefix.size());
        } else if (arg.rfind(cov_prefix, 0) == 0) {
            cov = arg.substr(cov_prefix.size());
        } else if (arg.rfind(module_prefix, 0) == 0) {
            const fs::path module(arg.substr(module_prefix.size()));
            module_ok = module.filename() == "engine module.dll" && fs::exists(module);
        } else {
            return 20;  // A path split into multiple arguments.
        }
    }
    if (!module_ok || xml.empty() || separator == 0 || separator + 3 != argc ||
        fs::path(argv[separator + 1]).filename() != "target program.exe" ||
        !fs::exists(argv[separator + 1]) || !fs::exists(argv[separator + 2])) {
        return 21;
    }
    const std::string file = fs::path(argv[separator + 2]).filename().string();
    if (file == "overlap failed.stp" || file == "overlap success.stp") {
        const auto directory=fs::path(argv[separator+2]).parent_path();
        Write(directory/(file+".started"),"started");
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while (!fs::exists(directory/(file+".release")) &&
               std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (!fs::exists(directory/(file+".release"))) return 22;
        if (file == "overlap failed.stp") return 1;
    }
    if (file == "failed process.stp") {
        return 1;
    }
    if (file == "missing report.stp") {
        return 0;
    }
    const std::string hits = file == "zero lines.stp" ? "0" : "1";
    Write(xml, "<coverage><sources><source>source root</source></sources><packages>"
               "<package><classes><class filename=\"sample.cpp\"><lines>"
               "<line number=\"7\" hits=\"" + hits + "\"/>"
               "<line number=\"7\" hits=\"" + hits + "\"/>"
               "<line number=\"8\" hits=\"" + hits + "\"/>"
               "</lines></class></classes></package></packages></coverage>");
    if (!cov.empty() && file != "missing cov.stp") {
        Write(cov, file == "empty cov.stp" ? std::string() : std::string("cov\0data", 8));
    }
    return 0;
}

coverage::FileCoverage Execute(coverage_plugin::ClientPlugin& client,
                               const std::string& run_id, const std::string& file,
                               int expected_exit, bool expected_return = true) {
    coverage::CoverageTask payload;
    payload.set_run_id(run_id);
    payload.add_files(file);
    common::Task task;
    task.set_task_id(run_id + "/" + file);
    payload.SerializeToString(task.mutable_payload());
    common::TaskResult result;
    CHECK(client.execute(task, result) == expected_return, "client reports durable summary status");
    CHECK(result.exit_code() == expected_exit, "task exit code reflects file success");
    coverage::CoverageResult decoded;
    CHECK(decoded.ParseFromString(result.result()), "result parses");
    CHECK(decoded.files_size() == 1, "one file result returned");
    return decoded.files_size() == 1 ? decoded.files(0) : coverage::FileCoverage();
}

fs::path PointPath(const fs::path& root, const std::string& run, const std::string& file) {
    return root / "store" / run / (std::to_string(common::Fnv1a64(file)) + ".json");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--quiet") {
        return FakeCoverage(argc, argv);
    }
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
                          ("coverage client test " + std::to_string(nonce));
    fs::create_directories(root / "store");
    fs::create_directories(root / "input stp");
    fs::create_directories(root / "coverage tool");
    fs::create_directories(root / "program dir");
    const fs::path executable = fs::absolute(argv[0]);
    const fs::path fake = root / "coverage tool" / executable.filename();
    fs::copy_file(executable, fake);
#ifdef _WIN32
    // The copied helper finds common.dll and its dependencies beside the
    // original test binary via the inherited PATH.
    const char* original_path = std::getenv("PATH");
    const std::string search_path = executable.parent_path().string() + ";" +
                                    (original_path ? original_path : "");
    _putenv_s("PATH", search_path.c_str());
#endif
    Write(root / "program dir" / "target program.exe", "fake target");
    Write(root / "program dir" / "engine module.dll", "fake module");
    const std::vector<std::string> files = {
        "ordinary file.stp", "zero lines.stp", "missing cov.stp", "empty cov.stp",
        "missing report.stp", "failed process.stp", "cov write fails.stp",
        "points write fails.stp", "add cov later.stp", "overlap failed.stp", "overlap success.stp"
    };
    for (const std::string& file : files) {
        Write(root / "input stp" / file, "stp");
    }

    nlohmann::json config = {
        {"dry_run", false}, {"store_type", "file"},
        {"store_dir", (root / "store").string()},
        {"stp_root", (root / "input stp").string()},
        {"program_dir", (root / "program dir").string()},
        {"export_dir", (root / "report dir").string()},
        {"opencppcoverage", fake.string()},
        {"target_exe", (root / "program dir" / "target program.exe").string()},
        {"engine_dlls", {(root / "program dir" / "engine module.dll").string()}},
        {"store_cov", true}
    };
    auto store = coverage_plugin::CreateCoverageResultStore(config.dump());
    CHECK(store->init(config.dump()), "file store init");
    coverage_plugin::ClientPlugin client;
    CHECK(client.init(config.dump()), "real execution client init");
    std::set<std::uint64_t> points;

    auto entry = Execute(client, "run", "ordinary file.stp", 0);
    CHECK(entry.ok() && entry.covered_lines() == 2, "spaced paths execute and duplicate lines dedupe");
    CHECK(store->loadFileCoverage("run", "ordinary file.stp", &points) && points.size() == 2,
          "raw point set persisted");
    CHECK(store->hasFileCov("run", "ordinary file.stp"), "binary persisted");

    entry = Execute(client, "run", "zero lines.stp", 0);
    CHECK(entry.ok() && entry.covered_lines() == 0, "zero-coverage execution is valid");
    CHECK(store->loadFileCoverage("run", "zero lines.stp", &points) && points.empty(),
          "empty point set persists and reloads");

    for (const std::string& file : {"ordinary file.stp", "zero lines.stp"}) {
        const fs::path path = PointPath(root, "run", file);
        const auto old_time = fs::file_time_type::clock::now() - std::chrono::hours(24);
        fs::last_write_time(path, old_time);
        const auto stored_time = fs::last_write_time(path);
        entry = Execute(client, "run", file, 0);
        CHECK(entry.ok(), "cached file succeeds");
        CHECK(fs::last_write_time(path) == stored_time, "cached point set is not rewritten");
    }

    // A report left by an older attempt must not make a successful process
    // that produced no current report appear successful.
    Write(root / "report dir" /
          (std::to_string(common::Fnv1a64("missing report.stp")) + ".xml"),
          "<coverage><packages/></coverage>");
    for (const std::string& file : {"missing cov.stp", "empty cov.stp",
                                    "missing report.stp", "failed process.stp"}) {
        entry = Execute(client, "run", file, 1);
        CHECK(!entry.ok(), "missing/empty report or failed process is reported as failure");
        CHECK(!store->loadFileCoverage("run", file, &points),
              "failed execution does not persist completed points");
    }

    coverage::CoverageTask mixed_payload;
    mixed_payload.set_run_id("run");
    mixed_payload.add_files("failed process.stp");
    mixed_payload.add_files("ordinary file.stp");
    common::Task mixed_task;
    mixed_payload.SerializeToString(mixed_task.mutable_payload());
    common::TaskResult mixed_result;
    CHECK(client.execute(mixed_task, mixed_result), "mixed task returns a complete summary");
    coverage::CoverageResult mixed_decoded;
    CHECK(mixed_result.exit_code() == 1 &&
          mixed_decoded.ParseFromString(mixed_result.result()) &&
          mixed_decoded.files_size() == 2 && !mixed_decoded.files(0).ok() &&
          mixed_decoded.files(1).ok(), "later success does not hide earlier failure");

    fs::create_directories(PointPath(root,"run","cov write fails.stp"));
    entry = Execute(client, "run", "cov write fails.stp", -1, false);
    CHECK(!entry.ok(), "cov storage failure is reported");
    CHECK(!store->loadFileCoverage("run", "cov write fails.stp", &points),
          "cov storage failure prevents a completed point record");

    fs::create_directories(PointPath(root, "run", "points write fails.stp"));
    entry = Execute(client, "run", "points write fails.stp", -1, false);
    CHECK(!entry.ok(), "point storage failure is reported");

    Write(PointPath(root,"run","add cov later.stp"), R"({"file":"add cov later.stp","points":[1]})");
    entry = Execute(client, "run", "add cov later.stp", 0);
    CHECK(entry.ok() && entry.covered_lines() == 2 && store->hasFileCov("run", "add cov later.stp"),
          "store_cov execution repairs cached points with a missing binary");

    // Force cache miss -> external execution -> another successful publication
    // -> this process completes. Both late failure and late success must report
    // the winner's three points, not this attempt's failure or two-point report.
    for (const std::string file : {"overlap failed.stp", "overlap success.stp"}) {
        coverage::CoverageTask payload;
        payload.set_run_id("overlap"); payload.add_files(file);
        common::Task task;
        task.set_run_id("overlap"); task.set_task_id(file); task.set_attempt_id("late");
        payload.SerializeToString(task.mutable_payload());
        common::TaskResult result;
        bool executed=false;
        std::thread worker([&] { executed=client.execute(task,result); });
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        const auto started=root/"input stp"/(file+".started");
        while (!fs::exists(started) && std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(fs::exists(started), "overlapping attempt really started after a cache miss");
        coverage_plugin::FileCoverageResult winner;
        winner.run_id="overlap"; winner.file=file; winner.attempt_id="winner";
        winner.successful=true; winner.points={17,23,29}; winner.cov="winning cov";
        CHECK(store->publish(winner), "other attempt publishes success during execution");
        Write(root/"input stp"/(file+".release"),"release");
        worker.join();
        coverage::CoverageResult decoded;
        CHECK(executed && result.exit_code()==0 && decoded.ParseFromString(result.result()) &&
                  decoded.files_size()==1 && decoded.files(0).ok() && decoded.files(0).covered_lines()==3,
              "task summary and exit code describe the committed winner after overlapping execution");
        CHECK(store->loadFileCoverage("overlap",file,&points) && points==std::set<std::uint64_t>({17,23,29}),
              "overlapping execution leaves the original successful points unchanged");
    }


    Write(PointPath(root, "run", "invalid json"), "{\"file\":12,\"points\":[1]}");
    CHECK(!store->loadFileCoverage("run", "invalid json", &points),
          "invalid persisted data is rejected without throwing");
    Write(PointPath(root, "run", "invalid json"), "{\"file\":\"invalid json\",\"points\":[-1]}");
    CHECK(!store->loadFileCoverage("run", "invalid json", &points), "reject negative points");

    config["keep_reports"] = true;
    coverage_plugin::ClientPlugin keep_client;
    CHECK(keep_client.init(config.dump()), "client retaining validation reports init");
    entry = Execute(keep_client, "kept-run", "ordinary file.stp", 0);
    const fs::path published = root / "report dir" /
        (std::to_string(common::Fnv1a64("ordinary file.stp")) + ".xml");
    CHECK(entry.ok() && fs::is_regular_file(published), "kept report uses legacy validator path");
    Write(published, "stale report");
    entry = Execute(keep_client, "next-kept-run", "ordinary file.stp", 0);
    std::string published_xml;
    CHECK(entry.ok() && common::ReadTextFile(published.string(), &published_xml) &&
          published_xml.rfind("<coverage>", 0) == 0,
          "successful report atomically replaces an old published report");

    std::error_code ec;
    fs::remove_all(root, ec);
    CHECK(!ec, "temporary execution artifacts cleaned");
    std::printf("test_coverage_client: %s\n", failures == 0 ? "OK" : "FAILED");
    return failures == 0 ? 0 : 1;
}
