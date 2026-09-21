#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <json.hpp>
#include <set>
#include <thread>

#include "common/AtomicFile.h"
#include "common/Manifest.h"
#include "common/PgUtil.h"
#include "common/TaskLedger.h"
#include "common/Util.h"
#include "coverage/ClientPlugin.h"
#include "coverage/CoverageStore.h"
#include "coverage/SchedulerPlugin.h"
#include "coverage.pb.h"
#include "stpcompare.pb.h"
#include "stpcompare/ClientPlugin.h"
#include "stpcompare/StpStore.h"
#include "stpcompare/SchedulerPlugin.h"
#include "stpcompare/StpUtil.h"
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
              manifest[0].path == "comma,file.stp" && manifest[0].has_size &&
              manifest[0].size == 12 && manifest[1].has_size,
          "last comma separates size");
    for (const auto& text : {"a.stp,1\na.stp,2\n", "a.stp,12x\n", "a.stp,-1\n", "{\"path\":3}\n",
                             "{\"path\":\"a\",\"size\":-1}\n",
                             "{\"path\":\"a\",\"content_sha256\":\"bad\"}\n"}) {
        write(path, text);
        CHECK(!common::ReadManifest(path.string(), &manifest), "bad manifest rejected");
    }
    const std::string digest(64, 'A');
    write(path, "{\"path\":\"comma,file.stp\",\"size_bytes\":5,\"content_sha256\":\"" +
                    digest + "\"}\n");
    CHECK(common::ReadManifest(path.string(), &manifest) && manifest[0].has_size &&
              manifest[0].size == 5 && manifest[0].content_sha256 == std::string(64, 'a'),
          "SHA-256 JSONL manifest");
    const nlohmann::json cfg = {{"store_dir", (root / "ledger").string()}, {"store_type", "file"}};
    common::TaskLedger ledger;
    CHECK(ledger.init(cfg.dump(), "coverage", "coverage_checkpoints"), "ledger init");
    common::TaskLedger custom;
    CHECK(custom.init(cfg.dump(), "third_party_plugin", "third_party_checkpoints"),
          "plugin-owned checkpoint table accepted");
    common::TaskLedger invalid;
    CHECK(!invalid.init(cfg.dump(), "third_party_plugin", "third_party;drop"),
          "invalid checkpoint table rejected");
    CHECK(ledger.planManifest("one", {task("one", 0)},
                              {{"f.stp",42,true,std::string(64,'a')}}, "fp"), "ledger plan");
    const auto stored_metadata = nlohmann::json::parse(
        read(root / "ledger" / "one" / "run-metadata.json"));
    CHECK(stored_metadata["file_metadata"][0]["size_bytes"] == 42 &&
              stored_metadata["file_metadata"][0]["content_sha256"] == std::string(64,'a'),
          "file-backed plan keeps manifest metadata");
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
    const auto completion = nlohmann::json::parse(read(journal));
    CHECK(completion["task_id"] == issued.task_id() && completion["exit_code"] == 0,
          "completion journal preserves the accepted outcome");
    common::TaskLedger recovered;
    CHECK(recovered.init(cfg.dump(), "coverage", "coverage_checkpoints"), "restore init");
    CHECK(recovered.restore("one") && recovered.finished(), "journal restores committed result");
    write(journal, issued.task_id());
    CHECK(recovered.restore("one") && recovered.finished(),
          "legacy task-only completion journal remains recoverable");
    const auto checkpoint = root / "ledger" / "one" / "checkpoint.bin";
#ifdef _WIN32
    for (const auto& locked_path : {checkpoint, journal}) {
        const auto file = CreateFileW(locked_path.c_str(), GENERIC_READ, 0, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(file != INVALID_HANDLE_VALUE, "exclusive recovery read fault installed");
        if (file != INVALID_HANDLE_VALUE) {
            bool blocked = false;
            try { recovered.restore("one"); }
            catch (const common::RecoveryBlocked&) { blocked = true; }
            CloseHandle(file);
            CHECK(blocked, "checkpoint or completion read fault blocks recovery");
            CHECK(recovered.restore("one") && recovered.finished(),
                  "recovery succeeds after the read fault clears");
        }
    }
#endif
    fs::remove(checkpoint);
    CHECK(!recovered.restore("one"), "confirmed missing checkpoint stays unrecoverable");
    write(checkpoint, "invalid checkpoint");
    CHECK(!recovered.restore("one"), "confirmed malformed checkpoint stays unrecoverable");
    write(checkpoint, before);
    write(journal, "");
    CHECK(!recovered.restore("one"), "confirmed malformed completion stays unrecoverable");
    write(journal, issued.task_id());
    CHECK(recovered.restore("one") && recovered.finished(), "recovery fixtures restored");
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
template<class Plugin, class Registration>
void schedulerVersionTests(const fs::path& root, const std::string& name,
                           const std::string& version = "v1") {
    const auto manifest = root / (name + "-manifest.txt");
    write(manifest, "sample.stp,3\n");
    nlohmann::json config = {{"store_type","file"}, {"store_dir",(root/name).string()},
        {"stp_manifest",manifest.string()}, {"expected_program_version",version},
        {"expected_dataset_version","d1"}};
    Plugin plugin;
    CHECK(plugin.init(config.dump()), "versioned plugin init");
    CHECK(!plugin.plan({name+"-conflict","d2",{}},{}), "configured dataset rejects conflicting run");
    CHECK(plugin.plan({name+"-run","d1",{}},{}), "matching dataset plan");
    Registration registration;
    registration.set_program_version(version); registration.set_dataset_version("d2");
    common::ClientInfo client; registration.SerializeToString(&client.registration_payload);
    common::Task issued;
    CHECK(!plugin.requestTask(client,{},issued), "pre-registered wrong dataset cannot receive task");
    registration.set_dataset_version("d1");
    registration.set_program_version("old-program-version");
    registration.SerializeToString(&client.registration_payload);
    common::ValidationResult validation;
    CHECK(plugin.validateClient(client,client.registration_payload,validation) && !validation.accepted,
          "old program version cannot register");
    CHECK(!plugin.requestTask(client,{},issued), "pre-registered old program cannot receive task");
    registration.set_program_version(version); registration.SerializeToString(&client.registration_payload);
    CHECK(plugin.validateClient(client,client.registration_payload,validation) && validation.accepted,
          "matching program version can register");
    CHECK(plugin.requestTask(client,{},issued), "matching client receives task");
    config["expected_program_version"]="v2";
    Plugin changed; CHECK(changed.init(config.dump()), "changed deployment init");
    bool blocked=false;
    try { changed.restore(name+"-run"); } catch(const common::RecoveryBlocked&) { blocked=true; }
    CHECK(blocked, "program change blocks recovery instead of mixing results");
    config["expected_program_version"]=version;
    Plugin resumed; CHECK(resumed.init(config.dump()) && resumed.restore(name+"-run"),
                          "original deployment remains recoverable");
    CHECK(resumed.requestTask(client,{},issued), "uncommitted task remains available");
    config.erase("expected_dataset_version");
    Plugin unconfigured; CHECK(unconfigured.init(config.dump()), "optional expected dataset");
    CHECK(unconfigured.plan({name+"-requested","d3",{}},{}), "request supplies dataset version");
    const auto metadata=nlohmann::json::parse(read(root/name/(name+"-requested")/"run-metadata.json"));
    CHECK(metadata["dataset_version"]=="d3", "request version persisted in plugin metadata");
}
void coverageClientVersionTests(const fs::path& root) {
    const auto directory=root/"coverage-program";
    fs::create_directory(directory);
    write(directory/"version.txt","manual-v1\n");
    nlohmann::json config={{"dry_run",true},{"store_type","file"},
        {"store_dir",(root/"coverage-client-version").string()},
        {"program_dir",directory.string()}};
    const std::string artifact_version="sha256:"+std::string(64,'a');
    for(const auto& version:{std::string(),artifact_version}) {
        config["program_version"]=version;
        coverage_plugin::ClientPlugin client;
        CHECK(client.init(config.dump()),"coverage client program version init");
        std::string bytes;
        coverage::CoverageClientInfo info;
        CHECK(client.collectRegistrationInfo(&bytes) && info.ParseFromString(bytes) &&
              info.program_version()==(version.empty()?"manual-v1":version),
              "coverage registration uses configured artifact identity or manual version fallback");
    }
    config.erase("program_version");
    coverage_plugin::ClientPlugin legacy;
    CHECK(legacy.init(config.dump()),"legacy coverage client init");
    std::string bytes;
    coverage::CoverageClientInfo info;
    CHECK(legacy.collectRegistrationInfo(&bytes) && info.ParseFromString(bytes) &&
          info.program_version()=="manual-v1","coverage absent override preserves version.txt");
    CHECK(read(directory/"version.txt")=="manual-v1\n",
          "artifact version injection does not modify packaged version.txt");
}
stpcompare::FileResult stpResult(const std::string& run,const std::string& file,
                                 const std::string& attempt) {
    stpcompare::FileResult result;
    result.run_id=run; result.file=file; result.attempt_id=attempt;
    result.successful=true;
    for(const auto& role:{"baseline","candidate"}) for(const auto& stage:{"first","second"}) {
        stpcompare::RawOutput output;
        output.program_role=role; output.export_stage=stage; output.program_version="v1";
        output.original_filename=std::string(stage)=="first"?"info.txt":"info_new.txt";
        output.content=std::string(role)+"-"+stage;
        output.content_size=output.content.size();
        output.content_sha256=common::Sha256Hex(output.content);
        result.outputs.push_back(std::move(output));
    }
    return result;
}
void stpStoreTests(const nlohmann::json& config) {
    auto store=stpcompare::CreateStpResultStore(config.dump());
    CHECK(store->init(config.dump()), "STP result store init");
    auto result=stpResult("stp-idempotent","cached.stp","first");
    CHECK(store->publish(result), "STP success published");
    CHECK(store->hasSuccessfulResult(result.run_id,result.file), "STP success can be reused");
    result.successful=false; result.detail="later attempt failed";
    CHECK(store->publish(result) && result.successful && result.detail.empty(),
          "STP publication returns durable success when a competing attempt failed");
}
void stpDualVersionTests(const fs::path& root) {
    const auto manifest=root/"stp-dual-manifest.txt";
    write(manifest,"one.stp,1\n");
    const nlohmann::json cfg={{"store_type","file"},{"store_dir",(root/"stp-dual-ledger").string()},
        {"stp_manifest",manifest.string()},{"expected_baseline_program_version","old-v1"},
        {"expected_candidate_program_version","new-v2"},{"expected_dataset_version","d1"}};
    stpcompare::SchedulerPlugin scheduler;
    CHECK(scheduler.init(cfg.dump()),"STP dual-version scheduler init");
    stpcompare::StpCompareClientInfo registration;
    registration.set_baseline_program_version("old-v1");
    registration.set_candidate_program_version("new-v2");
    registration.set_program_version("baseline=old-v1;candidate=new-v2");
    registration.set_dataset_version("d1");
    std::string bytes; registration.SerializeToString(&bytes);
    common::ValidationResult validation;
    CHECK(scheduler.validateClient({},bytes,validation) && validation.accepted,
          "matching baseline and candidate versions accepted");
    registration.set_candidate_program_version("wrong");
    registration.set_program_version("baseline=old-v1;candidate=wrong");
    registration.SerializeToString(&bytes);
    CHECK(scheduler.validateClient({},bytes,validation) && !validation.accepted,
          "candidate version mismatch rejected");
}
void stpArtifactVersionTests(const fs::path& root) {
    const auto manifest=root/"stp-artifact-manifest.txt";
    write(manifest,"one.stp,1\n");
    const std::string baseline="sha256:"+std::string(64,'a');
    const std::string candidate="sha256:"+std::string(64,'b');
    const nlohmann::json cfg={{"store_type","file"},{"store_dir",(root/"stp-artifact-ledger").string()},
        {"stp_manifest",manifest.string()},{"expected_baseline_program_version",baseline},
        {"expected_candidate_program_version",candidate},{"expected_dataset_version","d1"}};
    stpcompare::SchedulerPlugin scheduler;
    CHECK(scheduler.init(cfg.dump()) && scheduler.plan({"stp-artifact","d1",{}},{}),
          "STP artifact version plan");
    const auto dataset=root/"stp-artifact-data";
    fs::create_directory(dataset);
    write(dataset/"version.txt","d1\n");
    const nlohmann::json client_config={{"dry_run",true},{"store_type","file"},
        {"store_dir",(root/"stp-artifact-client").string()},{"stp_root",dataset.string()},
        {"programs",{{"baseline",{{"executable","baseline.exe"},{"version",baseline}}},
                     {"candidate",{{"executable","candidate.exe"},{"version",candidate}}}}}};
    stpcompare::ClientPlugin client_plugin;
    CHECK(client_plugin.init(client_config.dump()),"STP artifact client init");
    common::ClientInfo client;
    CHECK(client_plugin.collectRegistrationInfo(&client.registration_payload),"STP artifact registration");
    stpcompare::StpCompareClientInfo registration;
    CHECK(registration.ParseFromString(client.registration_payload) &&
          registration.baseline_program_version()==baseline &&
          registration.candidate_program_version()==candidate,
          "STP client reports both artifact identities");
    const auto matching=registration;
    common::ValidationResult validation;
    common::Task issued;
    for(int mismatch=0;mismatch<3;++mismatch) {
        registration=matching;
        if(mismatch==0) registration.set_baseline_program_version("old-baseline");
        if(mismatch==1) registration.set_candidate_program_version("old-candidate");
        if(mismatch==2) {
            registration.set_baseline_program_version(candidate);
            registration.set_candidate_program_version(baseline);
        }
        registration.SerializeToString(&client.registration_payload);
        CHECK(scheduler.validateClient(client,client.registration_payload,validation) && !validation.accepted,
              "STP old or exchanged program artifact cannot register");
        CHECK(!scheduler.requestTask(client,{},issued),
              "STP pre-registered old or exchanged artifact cannot receive task");
    }
    matching.SerializeToString(&client.registration_payload);
    CHECK(scheduler.validateClient(client,client.registration_payload,validation) && validation.accepted &&
          scheduler.requestTask(client,{},issued),"matching STP artifact pair receives task");
}
int fakeConverter(int argc, char** argv) {
    if (argc != 7 || std::string(argv[3])!="--input" ||
        std::string(argv[5])!="--output-dir" || !fs::exists(argv[4])) return 20;
    const auto role=std::string(argv[2]);
    const auto name = fs::path(argv[4]).filename().string();
    const auto outdir=fs::path(argv[6]);
    if(outdir.filename()!=role || !fs::is_directory(outdir)) return 21;
    if (name == "timeout.stp") std::this_thread::sleep_for(std::chrono::seconds(10));
    if (name == "missing.stp" && role=="candidate") return 0;
    std::ofstream first(outdir/"info.txt",std::ios::binary);
    first<<role; first.put('\0'); first<<"first"; first.close();
    std::ofstream second(outdir/"info_new.txt",std::ios::binary);
    second<<role; second.put('\0'); second<<"second"; second.close();
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
                                {"programs",{
                                    {"baseline",{{"executable",executable},{"version","old-v1"},
                                        {"arguments",{"--converter","baseline","--input","{stp}","--output-dir","{outdir}"}}}},
                                    {"candidate",{{"executable",executable},{"version","new-v2"},
                                        {"arguments",{"--converter","candidate","--input","{stp}","--output-dir","{outdir}"}}}}
                                }},
                                {"work_dir", (root / "work").string()},
                                {"keep_work", true},
                                {"process_timeout_ms", 1000}};
    stpcompare::ClientPlugin client;
    CHECK(client.init(cfg.dump()), "stp client init");
    std::string registration_bytes;
    stpcompare::StpCompareClientInfo registration;
    CHECK(client.collectRegistrationInfo(&registration_bytes) &&
          registration.ParseFromString(registration_bytes) &&
          registration.baseline_program_version()=="old-v1" &&
          registration.candidate_program_version()=="new-v2",
          "STP registration carries both program versions");
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
        if(expected_success) {
            CHECK(row["outputs"].size()==4,"four raw STP outputs stored");
            std::set<std::string> pairs;
            for(const auto& output:row["outputs"])
                pairs.insert(output["program_role"].get<std::string>()+":"+
                             output["export_stage"].get<std::string>());
            const std::set<std::string> expected_pairs = {
                "baseline:first","baseline:second","candidate:first","candidate:second"};
            CHECK(pairs==expected_pairs,
                  "STP role and stage identities stored");
        } else {
            CHECK(row["outputs"].empty(),"partial converter output is not published");
        }
    }
    common::CancellationSource source;
    source.requestCancellation();
    common::Task empty;
    common::TaskResult ignored;
    CHECK(!client.execute(empty, ignored, source.token()), "cancelled task cannot publish");
}
#ifdef _WIN32
void postgresTests(const std::string& connection) {
    common::PgConnection database;
    CHECK(database.connect(connection), "postgres inspection connection");
    auto conn = database.get();
    CHECK(common::PgExec(conn,
        "CREATE TABLE plugin_run_files(run_id text NOT NULL,plugin_type text NOT NULL,file text NOT NULL,"
        "PRIMARY KEY(run_id,plugin_type,file));"
        "CREATE TABLE stp_compare_results(run_id text NOT NULL,file text NOT NULL,"
        "info_txt text,info_new_txt text,PRIMARY KEY(run_id,file));"
        "INSERT INTO stp_compare_results VALUES('legacy','old.stp','old','new')", {}),
        "legacy run-file schema fixture");
    const nlohmann::json cfg = {{"store_type", "postgres"}, {"db_conn", connection}};
    stpStoreTests(cfg);
    auto raw = common::PgQuery(
        conn,"SELECT count(*)::text,count(DISTINCT program_role||':'||export_stage)::text,"
             "count(DISTINCT attempt_id)::text FROM stp_compare_results "
             "WHERE run_id='stp-idempotent' AND file='cached.stp'");
    CHECK(raw && common::PgCell(raw.get(),0,0)=="4" && common::PgCell(raw.get(),0,1)=="4" &&
          common::PgCell(raw.get(),0,2)=="1","four STP raw outputs share one attempt");
    raw=common::PgQuery(conn,"SELECT count(*)::text FROM stp_compare_results WHERE run_id='legacy' "
        "AND info_txt='old' AND program_role IS NULL");
    CHECK(raw && common::PgCell(raw.get(),0,0)=="1","legacy STP rows preserved during schema migration");
    raw=common::PgQuery(conn,"SELECT count(*)::text FROM stp_compare_results WHERE "
        "content_size=octet_length(content) AND content_sha256 ~ '^[0-9a-f]{64}$'");
    CHECK(raw && common::PgCell(raw.get(),0,0)=="4","STP raw sizes and SHA-256 persisted");
    CHECK(common::PgExec(conn,
        "CREATE FUNCTION fail_stp_raw() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN "
        "IF NEW.program_role='candidate' THEN RAISE EXCEPTION 'injected stp'; END IF; RETURN NEW; END; $$;"
        "CREATE TRIGGER injected_stp BEFORE INSERT ON stp_compare_results FOR EACH ROW "
        "EXECUTE FUNCTION fail_stp_raw()",{}),"inject STP atomic failure");
    auto stp_store=stpcompare::CreateStpResultStore(cfg.dump());
    CHECK(stp_store->init(cfg.dump()),"second STP postgres store init");
    auto rollback_stp=stpResult("stp-rollback","rollback.stp","attempt-x");
    CHECK(!stp_store->publish(rollback_stp),"partial STP raw publication rejected");
    raw=common::PgQuery(conn,"SELECT (SELECT count(*) FROM stp_compare_results WHERE run_id='stp-rollback')::text,"
        "(SELECT count(*) FROM plugin_file_outcomes WHERE run_id='stp-rollback')::text");
    CHECK(raw && common::PgCell(raw.get(),0,0)=="0" && common::PgCell(raw.get(),0,1)=="0",
          "STP raw outputs and success outcome roll back together");
    CHECK(common::PgExec(conn,"DROP TRIGGER injected_stp ON stp_compare_results",{}),
          "remove STP failure trigger");
    auto first = coverage_plugin::CreateCoverageResultStore(cfg.dump()),
         second = coverage_plugin::CreateCoverageResultStore(cfg.dump());
    CHECK(first->init(cfg.dump()) && second->init(cfg.dump()), "postgres stores initialized");
    auto migrated_columns = common::PgQuery(conn,
        "SELECT count(*)::text FROM pg_attribute WHERE attrelid='plugin_run_files'::regclass "
        "AND attname IN ('size_bytes','content_sha256') AND NOT attisdropped");
    CHECK(migrated_columns && common::PgCell(migrated_columns.get(),0,0) == "2",
          "legacy run-file schema gains optional manifest metadata");
    const nlohmann::json ledger_cfg = {{"store_type", "postgres"}, {"db_conn", connection}};
    common::TaskLedger custom_ledger;
    CHECK(custom_ledger.init(ledger_cfg.dump(), "third_party_plugin", "Third_Party_Checkpoints"),
          "postgres plugin-owned checkpoint table initialized");
    auto registered_checkpoint = common::PgQuery(
        conn, "SELECT count(*)::text FROM plugin_checkpoint_tables "
              "WHERE plugin_type='third_party_plugin' AND checkpoint_table='third_party_checkpoints'");
    CHECK(registered_checkpoint && common::PgCell(registered_checkpoint.get(), 0, 0) == "1",
          "plugin checkpoint table registered for maintenance");
    CHECK(custom_ledger.planManifest("custom-ledger", {task("custom-ledger", 0)},
                                     {{"f.stp",42,true,std::string(64,'b')}}, "fp"),
          "postgres plugin-owned checkpoint plan saved");
    auto custom_checkpoint = common::PgQuery(
        conn, "SELECT count(*)::text FROM third_party_checkpoints WHERE run_id='custom-ledger'");
    CHECK(custom_checkpoint && common::PgCell(custom_checkpoint.get(), 0, 0) == "1",
          "custom checkpoint table contains plan");
    auto file_metadata = common::PgQuery(
        conn, "SELECT size_bytes::text,content_sha256 FROM plugin_run_files "
              "WHERE run_id='custom-ledger' AND plugin_type='third_party_plugin'");
    CHECK(file_metadata && common::PgCell(file_metadata.get(),0,0) == "42" &&
              common::PgCell(file_metadata.get(),0,1) == std::string(64,'b'),
          "postgres plan keeps file size and SHA-256");
    common::TaskLedger recovered_ledger;
    CHECK(recovered_ledger.init(ledger_cfg.dump(), "third_party_plugin", "third_party_checkpoints") &&
              recovered_ledger.restore("custom-ledger"),
          "custom checkpoint table restores plan");
    registered_checkpoint = common::PgQuery(
        conn, "SELECT count(*)::text FROM plugin_checkpoint_tables WHERE plugin_type='third_party_plugin'");
    CHECK(registered_checkpoint && common::PgCell(registered_checkpoint.get(), 0, 0) == "1",
          "reinitialization keeps one canonical checkpoint registration");
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
    CHECK(common::Sha256Hex("")==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "SHA-256 empty vector");
    CHECK(common::Sha256Hex("abc")==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256 abc vector");
    ledgerTests(root);
    resultTests(root);
    stpStoreTests({{"store_type","file"},{"store_dir",(root/"stp-idempotent").string()}});
    stpDualVersionTests(root);
    stpArtifactVersionTests(root);
    coverageClientVersionTests(root);
    schedulerVersionTests<coverage_plugin::SchedulerPlugin,coverage::CoverageClientInfo>(root,"coverage");
    schedulerVersionTests<coverage_plugin::SchedulerPlugin,coverage::CoverageClientInfo>(
        root,"coverage-artifact","sha256:"+std::string(64,'a'));
    schedulerVersionTests<stpcompare::SchedulerPlugin,stpcompare::StpCompareClientInfo>(root,"stpcompare");
    stpClientTests(root, fs::absolute(argv[0]).string());
    std::error_code error;
    fs::remove_all(root, error);
    CHECK(!error, "owned test directory cleaned");
    std::printf("plugin runtime: %s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
