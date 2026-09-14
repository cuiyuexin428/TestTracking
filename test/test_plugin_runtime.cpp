#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <json.hpp>
#include <thread>

#include "common/AtomicFile.h"
#include "common/Manifest.h"
#include "common/PgUtil.h"
#include "common/TaskLedger.h"
#include "common/Util.h"
#include "coverage/CoverageStore.h"
#include "stpcompare.pb.h"
#include "stpcompare/ClientPlugin.h"
#include "stpcompare/StpStore.h"
#ifdef _WIN32
#include <windows.h>
#endif
namespace fs = std::filesystem;
namespace {
int failures = 0;
#define CHECK(value, description)                                            \
    do {                                                                     \
        if (!(value)) {                                                      \
            ++failures;                                                      \
            std::fprintf(stderr, "FAIL: %s at %d\n", description, __LINE__); \
        }                                                                    \
    } while (0)
void write(const fs::path& path, const std::string& bytes) {
    CHECK(common::WriteAtomicFile(path, bytes), "fixture write");
}
std::string read(const fs::path& path) {
    std::string bytes;
    CHECK(common::ReadCompleteFile(path, &bytes), "fixture read");
    return bytes;
}
common::Task task(const std::string& run, int index) {
    common::Task value;
    value.set_run_id(run);
    value.set_task_id(run + "-t" + std::to_string(index));
    value.set_payload("payload");
    return value;
}
common::TaskResult report(const common::Task& task) {
    common::TaskResult result;
    result.set_task_id(task.task_id());
    result.set_run_id(task.run_id());
    return result;
}
void ledgerTests(const fs::path& root) {
    std::vector<common::ManifestEntry> manifest;
    std::string fingerprint;
    const auto path = root / "manifest";
    write(path, "comma,file.stp,12\nother.stp,0\n");
    CHECK(common::ReadManifest(path.string(), &manifest, &fingerprint) &&
              manifest[0].path == "comma,file.stp",
          "last comma separates size");
    for (const auto& text : {"a.stp,1\na.stp,2\n", "a.stp,12x\n", "a.stp,-1\n", "{\"path\":3}\n",
                             "{\"path\":\"a\",\"size\":-1}\n"}) {
        write(path, text);
        CHECK(!common::ReadManifest(path.string(), &manifest), "bad manifest rejected");
    }
    write(path, "{\"path\":\"comma,file.stp\",\"size\":5}\n");
    CHECK(common::ReadManifest(path.string(), &manifest), "JSONL manifest");
    const nlohmann::json cfg = {{"store_dir", (root / "ledger").string()}, {"store_type", "file"}};
    common::TaskLedger ledger;
    CHECK(ledger.init(cfg.dump(), "coverage"), "ledger init");
    CHECK(ledger.plan("one", {task("one", 0)}, {"f.stp"}, "fp"), "ledger plan");
    common::Task issued;
    CHECK(ledger.request(&issued), "request task");
    const auto before = read(root / "ledger" / "one" / "checkpoint.bin");
    const auto journal = root / "ledger" / "one" / "completed" /
                         (std::to_string(common::Fnv1a64(issued.task_id())) + ".done");
    fs::create_directories(journal);
    CHECK(!ledger.commit(report(issued)) && !ledger.finished(),
          "failed durable commit never finishes");
    fs::remove(journal);
    const auto metadata = root / "ledger" / "one" / "run-metadata.json";
    const auto old_metadata = read(metadata);
    fs::remove(metadata);
    fs::create_directory(metadata);
    CHECK(!ledger.commit(report(issued)) && !ledger.finished(),
          "final metadata failure stays pending");
    fs::remove(metadata);
    write(metadata, old_metadata);
    CHECK(ledger.commit(report(issued)) && ledger.finished(),
          "duplicate result retries final commit");
    CHECK(ledger.commit(report(issued)), "committed duplicate is idempotent");
    CHECK(read(root / "ledger" / "one" / "checkpoint.bin") == before,
          "completion does not rewrite plan");
    common::TaskLedger recovered;
    CHECK(recovered.init(cfg.dump(), "coverage"), "restore init");
    CHECK(recovered.restore("one") && recovered.finished(), "journal restores committed result");
    auto wrong = report(issued);
    wrong.set_run_id("another");
    CHECK(!recovered.commit(wrong), "old generation rejected");
    CHECK(ledger.plan("empty", {}, {"already-done.stp"}, "legacy") && ledger.finished(),
          "complete legacy restore finishes");

    for (int count : {1000, 10000}) {
        const auto run = "scale-" + std::to_string(count);
        std::vector<common::Task> tasks;
        std::vector<std::string> files;
        for (int i = 0; i < count; ++i) {
            tasks.push_back(task(run, i));
            files.push_back(std::to_string(i) + ".stp");
        }
        const auto start = std::chrono::steady_clock::now();
        CHECK(ledger.plan(run, tasks, files, "scale"), "large plan");
        const auto plan = read(root / "ledger" / run / "checkpoint.bin");
        for (int i = 0; i < 1000; ++i) {
            common::Task issued;
            CHECK(ledger.request(&issued), "scale dispatch");
            CHECK(ledger.commit(report(issued)), "scale commit");
        }
        CHECK(read(root / "ledger" / run / "checkpoint.bin") == plan,
              "large plan remains immutable");
        CHECK(recovered.restore(run), "large recovery");
        int remaining = 0;
        while (recovered.request(&issued)) ++remaining;
        CHECK(remaining == count - 1000, "recovery uses saved plan and completion journal");
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        std::printf(
            "ledger scale: planned=%d committed=1000 remaining=%d elapsed_ms=%lld plan_bytes=%zu\n",
            count, remaining, ms, plan.size());
    }
}
void resultTests(const fs::path& root) {
    const nlohmann::json cfg = {{"store_dir", (root / "results").string()}, {"store_type", "file"}};
    auto first = coverage_plugin::CreateCoverageResultStore(cfg.dump());
    auto second = coverage_plugin::CreateCoverageResultStore(cfg.dump());
    CHECK(first->init(cfg.dump()) && second->init(cfg.dump()), "result store init");
    coverage_plugin::FileCoverageResult a, b;
    a.run_id = "atomic";
    a.file = "same.stp";
    a.attempt_id = "a";
    a.successful = true;
    a.points = {1, 2};
    a.cov = "A";
    b = a;
    b.attempt_id = "b";
    b.points = {5, 6, 7};
    b.cov = "B";
    std::atomic<bool> start{false};
    bool ok_a = false, ok_b = false;
    std::thread one([&] {
        while (!start.load()) std::this_thread::yield();
        ok_a = first->publish(a);
    });
    std::thread two([&] {
        while (!start.load()) std::this_thread::yield();
        ok_b = second->publish(b);
    });
    start = true;
    one.join();
    two.join();
    CHECK(ok_a && ok_b, "concurrent publishes accepted idempotently");
    const auto path =
        root / "results" / "atomic" / (std::to_string(common::Fnv1a64(a.file)) + ".json");
    auto row = nlohmann::json::parse(read(path));
    CHECK((row["attempt_id"] == "a" && row["points"] == a.points &&
           row["cov"] == std::vector<unsigned char>{'A'}) ||
              (row["attempt_id"] == "b" && row["points"] == b.points &&
               row["cov"] == std::vector<unsigned char>{'B'}),
          "points and binary come from one winning attempt");
    const auto winner = read(path);
    a.successful = false;
    a.detail = "late failure";
    CHECK(first->publish(a) && read(path) == winner,
          "late failure never overwrites accepted success");
    a.file = "retry.stp";
    CHECK(first->publish(a), "failure outcome recorded");
    a.successful = true;
    CHECK(second->publish(a), "failed result may retry successfully");
}
int fakeConverter(int argc, char** argv) {
    if (argc != 3 || !fs::exists(argv[2])) return 20;
    const auto name = fs::path(argv[2]).filename().string();
    if (name == "timeout.stp") std::this_thread::sleep_for(std::chrono::seconds(10));
    if (name == "missing.stp") return 0;
    std::ofstream("info.txt") << "current";
    std::ofstream("info_new.txt") << "converted";
    return 0;
}
void stpClientTests(const fs::path& root, const std::string& executable) {
    fs::create_directories(root / "input files");
    fs::create_directories(root / "work");
    for (const auto& file : {"space , file.stp", "case-{outdir}.stp", "missing.stp", "timeout.stp"})
        write(root / "input files" / file, "STP");
    const nlohmann::json cfg = {{"dry_run", false},
                                {"store_type", "file"},
                                {"store_dir", (root / "stp-store").string()},
                                {"stp_root", (root / "input files").string()},
                                {"program_exe", executable},
                                {"program_args", {"--converter", "{stp}"}},
                                {"work_dir", (root / "work").string()},
                                {"keep_work", true},
                                {"process_timeout_ms", 1000}};
    stpcompare::ClientPlugin client;
    CHECK(client.init(cfg.dump()), "stp client init");
    for (const auto& file :
         {"space , file.stp", "case-{outdir}.stp", "missing.stp", "timeout.stp"}) {
        stpcompare::StpCompareTask payload;
        payload.set_run_id("stp");
        payload.add_files(file);
        common::Task work;
        work.set_run_id("stp");
        work.set_attempt_id("attempt");
        payload.SerializeToString(work.mutable_payload());
        common::TaskResult result;
        CHECK(client.execute(work, result), "business outcomes persisted");
        const bool expected_success =
            std::string(file) == "space , file.stp" || std::string(file) == "case-{outdir}.stp";
        CHECK(result.exit_code() == (expected_success ? 0 : 1), "STP result exit matches outcome");
        const auto path =
            root / "stp-store" / "stp" / (std::to_string(common::Fnv1a64(file)) + ".json");
        const auto row = nlohmann::json::parse(read(path));
        CHECK(row["status"] == (expected_success ? "success" : "failed"),
              "success and failure rows recorded");
    }
    common::CancellationSource source;
    source.requestCancellation();
    common::Task empty;
    common::TaskResult ignored;
    CHECK(!client.execute(empty, ignored, source.token()), "cancelled task cannot publish");
}
#ifdef _WIN32
void postgresTests(const std::string& connection) {
    const nlohmann::json cfg = {{"store_type", "postgres"}, {"db_conn", connection}};
    auto first = coverage_plugin::CreateCoverageResultStore(cfg.dump()),
         second = coverage_plugin::CreateCoverageResultStore(cfg.dump());
    CHECK(first->init(cfg.dump()) && second->init(cfg.dump()), "postgres stores initialized");
    common::PgConnection database;
    CHECK(database.connect(connection), "postgres inspection connection");
    auto conn = database.get();
    coverage_plugin::FileCoverageResult a, b;
    a.run_id = "atomic-db";
    a.file = "file.stp";
    a.attempt_id = "A";
    a.successful = true;
    a.points = {11, 12};
    a.cov = "AAA";
    b = a;
    b.attempt_id = "B";
    b.points = {21, 22, 23};
    b.cov = "BBB";
    bool ok_a = false, ok_b = false;
    std::thread one([&] { ok_a = first->publish(a); }), two([&] { ok_b = second->publish(b); });
    one.join();
    two.join();
    CHECK(ok_a && ok_b, "postgres concurrent publication");
    auto row =
        common::PgQuery(conn,
                        "SELECT o.attempt_id,encode(c.cov,'escape'),octet_length(p.points)::text "
                        "FROM plugin_file_outcomes o "
                        "JOIN coverage_cov_files c USING(run_id,file) JOIN coverage_file_points p "
                        "USING(run_id,file) "
                        "WHERE o.run_id='atomic-db'");
    CHECK(row && PQntuples(row.get()) == 1, "one complete atomic row");
    const auto attempt = common::PgCell(row.get(), 0, 0);
    CHECK(common::PgCell(row.get(), 0, 1) == (attempt == "A" ? "AAA" : "BBB") &&
              common::PgCell(row.get(), 0, 2) == (attempt == "A" ? "16" : "24"),
          "cov and points same database attempt");
    CHECK(common::PgExec(conn,
                         "CREATE FUNCTION fail_cov() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN "
                         "RAISE EXCEPTION 'injected'; END; $$;"
                         "CREATE TRIGGER injected BEFORE INSERT ON coverage_cov_files FOR EACH ROW "
                         "EXECUTE FUNCTION fail_cov()",
                         {}),
          "inject atomic failure");
    a.file = "rollback.stp";
    CHECK(!first->publish(a), "failed cov write aborts publication");
    auto count = common::PgQuery(
        conn, "SELECT count(*)::text FROM coverage_file_points WHERE file='rollback.stp'");
    CHECK(common::PgCell(count.get(), 0, 0) == "0", "points rolled back with cov failure");
    count = common::PgQuery(
        conn, "SELECT count(*)::text FROM plugin_file_outcomes WHERE file='rollback.stp'");
    CHECK(common::PgCell(count.get(), 0, 0) == "0", "outcome rolled back with cov failure");
    CHECK(common::PgExec(conn, "DROP TRIGGER injected ON coverage_cov_files", {}),
          "remove owned failure trigger");
    CHECK(first->publish(a), "publication recovers after failure");
    // Kill only this test store's idle connection and verify the next operation
    // fails safely or reconnects; a later idempotent retry must succeed.
    auto terminated = common::PgQuery(
        conn,
        "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname=current_database() "
        "AND pid<>pg_backend_pid() AND state='idle'");
    a.file = "after-disconnect.stp";
    first->publish(a);
    CHECK(first->publish(a), "disconnected connection recovers without statement replay");
    a.file = "failed.stp";
    a.successful = false;
    a.detail = "process failed";
    CHECK(first->publish(a), "failed database outcome stored");
    auto failed = common::PgQuery(
        conn, "SELECT status,detail FROM plugin_file_outcomes WHERE file='failed.stp'");
    CHECK(common::PgCell(failed.get(), 0, 0) == "failed" &&
              common::PgCell(failed.get(), 0, 1) == "process failed",
          "failure reason retained");
}
#endif
}  // namespace
int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--converter") return fakeConverter(argc, argv);
#ifdef _WIN32
    if (argc == 3 && std::string(argv[1]) == "--db") {
        postgresTests(argv[2]);
        return failures ? 1 : 0;
    }
#endif
    const auto root = fs::temp_directory_path() /
                      ("plugin-runtime-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    ledgerTests(root);
    resultTests(root);
    stpClientTests(root, fs::absolute(argv[0]).string());
    std::error_code error;
    fs::remove_all(root, error);
    CHECK(!error, "owned test directory cleaned");
    std::printf("plugin runtime: %s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
