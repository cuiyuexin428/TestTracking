// Standalone database post-processor; the plugin retains raw results only.
// Default: deterministic greedy cover. Optional --merge-cov also merges OCC reports.
#include "coverage/CoveragePostProcess.h"
#include "coverage/CoverageUtil.h"
#include "coverage.pb.h"
#include "common/Logger.h"
#include "common/PgUtil.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <libpq-fe.h>
#endif

namespace {
struct Args {
    std::string run_id, db_conn, workdir;
    std::string opencppcoverage = "OpenCppCoverage.exe";
    bool merge_cov = false;
};

bool ParseArgs(int argc, char** argv, Args* out) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&](std::string* value) {
            if (i + 1 >= argc) return false;
            *value = argv[++i];
            return !value->empty();
        };
        if (arg == "--run") {
            if (!next(&out->run_id)) return false;
        } else if (arg == "--db") {
            if (!next(&out->db_conn)) return false;
        } else if (arg == "--merge-cov") {
            out->merge_cov = true;
        } else if (arg == "--opencppcoverage") {
            if (!next(&out->opencppcoverage)) return false;
        } else if (arg == "--workdir") {
            if (!next(&out->workdir)) return false;
        } else {
            LOG_ERROR("coverage_settle: unknown arg " << arg);
            return false;
        }
    }
    return !out->run_id.empty() && !out->db_conn.empty();
}

#ifdef _WIN32
using Connection = std::unique_ptr<PGconn, decltype(&PQfinish)>;
using QueryResult = std::unique_ptr<PGresult, decltype(&PQclear)>;

QueryResult Query(PGconn* conn, const char* sql, const std::string& run_id,
                  bool binary = false) {
    const char* values[] = {run_id.c_str()};
    QueryResult result(PQexecParams(conn, sql, 1, nullptr, values, nullptr, nullptr,
                                    binary ? 1 : 0), PQclear);
    if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK) {
        LOG_ERROR("coverage_settle: query failed: " << PQerrorMessage(conn));
        result.reset();
    }
    return result;
}

std::string Cell(PGresult* result, int row, int column) {
    return std::string(PQgetvalue(result, row, column),
                       static_cast<size_t>(PQgetlength(result, row, column)));
}

struct Transaction {
    PGconn* conn;
    bool active = false;
    ~Transaction() {
        if (active) common::PgExec(conn, "ROLLBACK", {});
    }
    bool begin() {
        active = common::PgExec(conn, "BEGIN ISOLATION LEVEL REPEATABLE READ", {});
        return active;
    }
    bool commit() {
        if (!common::PgExec(conn, "COMMIT", {})) return false;
        active = false;
        return true;
    }
};

// Remove only the child directory that this invocation created, never the
// caller's --workdir. Run IDs are not used to construct filesystem paths.
struct WorkDirectory {
    std::filesystem::path path;
    ~WorkDirectory() {
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    }
    bool create(const std::string& parent) {
        const auto base = parent.empty() ? std::filesystem::temp_directory_path()
                                         : std::filesystem::absolute(parent);
        std::error_code ec;
        std::filesystem::create_directories(base, ec);
        if (ec) return false;
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 100; ++attempt) {
            const auto candidate = base / ("coverage-settle-" + std::to_string(nonce) +
                                           "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(candidate, ec)) {
                path = candidate;
                return true;
            }
            if (ec) return false;
        }
        return false;
    }
};

bool EnsureDerivedSchema(PGconn* conn) {
    const char* ddl =
        "CREATE TABLE IF NOT EXISTS coverage_selected ("
        " run_id text NOT NULL, file text NOT NULL,"
        " selected_at timestamptz DEFAULT now(), PRIMARY KEY (run_id, file));"
        "CREATE TABLE IF NOT EXISTS coverage_run_summary ("
        " run_id text PRIMARY KEY, total_files int, selected_files int,"
        " cumulative_points bigint, covered_lines bigint, total_lines bigint,"
        " percent double precision, finished_at timestamptz);"
        "CREATE TABLE IF NOT EXISTS coverage_cumulative_cov ("
        " run_id text PRIMARY KEY, cov bytea, updated_at timestamptz DEFAULT now());";
    if (!common::PgInitializeSchema(conn, ddl)) {
        LOG_ERROR("coverage_settle: schema init failed: " << PQerrorMessage(conn));
        return false;
    }
    return true;
}

bool BackfillLegacyRun(PGconn* conn,const Args& args) {
    auto exists=Query(conn,"SELECT to_regclass('coverage_checkpoints') IS NOT NULL WHERE $1::text IS NOT NULL",args.run_id);
    if(!exists || Cell(exists.get(),0,0)!="t") return false;
    auto checkpoint=Query(conn,"SELECT blob FROM coverage_checkpoints WHERE run_id=$1",args.run_id,true);
    coverage::CoverageCheckpoint state;
    if(!checkpoint || PQntuples(checkpoint.get())!=1 ||
       !state.ParseFromString(Cell(checkpoint.get(),0,0)) || state.checkpoint_version()!=3 ||
       state.total_files()<=0 || state.remaining_tasks_size()!=0 || state.priority_tasks_size()!=0) return false;
    auto count=Query(conn,"SELECT count(*)::text FROM coverage_file_points WHERE run_id=$1",args.run_id);
    if(!count || Cell(count.get(),0,0)!=std::to_string(state.total_files())) return false;
    return common::PgExec(conn,
        "INSERT INTO plugin_runs(run_id,plugin_type,expected_files,manifest_fingerprint,status,finished_at) "
        "VALUES($1,'coverage',$2,$3,'finished',now())",
        {{args.run_id,false},{std::to_string(state.total_files()),false},{state.manifest_fingerprint(),false}}) &&
        common::PgExec(conn,
        "INSERT INTO plugin_run_files(run_id,plugin_type,file) SELECT run_id,'coverage',file "
        "FROM coverage_file_points WHERE run_id=$1 ON CONFLICT DO NOTHING",{{args.run_id,false}}) &&
        common::PgExec(conn,
        "INSERT INTO plugin_file_outcomes(run_id,plugin_type,file,attempt_id,status,detail) "
        "SELECT run_id,'coverage',file,'legacy-v3','success','migrated complete v3 run' "
        "FROM coverage_file_points WHERE run_id=$1 ON CONFLICT DO NOTHING",{{args.run_id,false}});
}

bool LoadCompleteRun(PGconn* conn,const Args& args,
                      std::unordered_map<std::string,std::set<std::uint64_t>>* files) {
    auto run=Query(conn,"SELECT expected_files::text,status FROM plugin_runs "
                        "WHERE run_id=$1 AND plugin_type='coverage'",args.run_id);
    if(!run) return false;
    if(PQntuples(run.get())==0) {
        if(!BackfillLegacyRun(conn,args)) {
            LOG_ERROR("coverage_settle: missing durable manifest or complete legacy checkpoint"); return false;
        }
        run=Query(conn,"SELECT expected_files::text,status FROM plugin_runs "
                       "WHERE run_id=$1 AND plugin_type='coverage'",args.run_id);
    }
    if(!run || PQntuples(run.get())!=1 || Cell(run.get(),0,1)!="finished") {
        LOG_ERROR("coverage_settle: run execution is not durably finished"); return false;
    }
    const auto expected=std::stoll(Cell(run.get(),0,0));
    auto points=Query(conn,
        "SELECT m.file,p.points,o.status FROM plugin_run_files m "
        "LEFT JOIN coverage_file_points p ON p.run_id=m.run_id AND p.file=m.file "
        "LEFT JOIN plugin_file_outcomes o ON o.run_id=m.run_id AND o.plugin_type=m.plugin_type AND o.file=m.file "
        "WHERE m.run_id=$1 AND m.plugin_type='coverage'",args.run_id,true);
    if(!points || expected<=0 || PQntuples(points.get())!=expected) {
        LOG_ERROR("coverage_settle: incomplete durable manifest"); return false;
    }
    auto raw_count=Query(conn,"SELECT count(*)::text FROM coverage_file_points WHERE run_id=$1",args.run_id);
    if(!raw_count || Cell(raw_count.get(),0,0)!=std::to_string(expected)) {
        LOG_ERROR("coverage_settle: raw result count differs from manifest"); return false;
    }
    for(int row=0;row<PQntuples(points.get());++row) {
        const auto file=Cell(points.get(),row,0), blob=Cell(points.get(),row,1);
        if(file.empty() || PQgetisnull(points.get(),row,1) || blob.size()%8!=0 ||
           Cell(points.get(),row,2)!="success") {
            LOG_ERROR("coverage_settle: missing, failed or malformed raw result for "<<file); return false;
        }
        const auto unpacked=coverage_plugin::UnpackPoints(blob);
        auto& set=(*files)[file]; set.insert(unpacked.begin(),unpacked.end());
    }
    return true;
}

bool MergeSelected(PGconn* conn, const Args& args, size_t selected_count,
                   std::string* merged, std::int64_t* covered, std::int64_t* total) {
    auto covs = Query(conn,
        "SELECT c.file, c.cov FROM coverage_cov_files c "
        "JOIN coverage_selected s ON s.run_id=c.run_id AND s.file=c.file "
        "WHERE c.run_id=$1 ORDER BY c.file", args.run_id, true);
    if (!covs) return false;
    if (static_cast<size_t>(PQntuples(covs.get())) != selected_count) {
        LOG_ERROR("coverage_settle: --merge-cov requires a binary report for every selected file");
        return false;
    }
    WorkDirectory work;
    if (!work.create(args.workdir)) {
        LOG_ERROR("coverage_settle: cannot create a temporary merge directory");
        return false;
    }
    std::vector<std::string> paths;
    for (int row = 0; row < PQntuples(covs.get()); ++row) {
        if (PQgetisnull(covs.get(), row, 1) || PQgetlength(covs.get(), row, 1) <= 0) {
            LOG_ERROR("coverage_settle: empty binary report for " << Cell(covs.get(), row, 0));
            return false;
        }
        const auto path = work.path / ("input-" + std::to_string(row) + ".cov");
        std::ofstream out(path, std::ios::binary);
        out.write(PQgetvalue(covs.get(), row, 1), PQgetlength(covs.get(), row, 1));
        out.close();
        if (!out) return false;
        paths.push_back(path.string());
    }
    covs.reset();
    const auto out_cov = work.path / "cumulative.cov";
    const auto out_xml = work.path / "summary.xml";
    if (!coverage_plugin::MergeCoverageFiles(args.opencppcoverage, paths,
                                              out_cov.string(), out_xml.string()) ||
        !coverage_plugin::ParseCoverageTotals(out_xml.string(), covered, total)) {
        LOG_ERROR("coverage_settle: cov merge/report validation failed");
        return false;
    }
    std::ifstream input(out_cov, std::ios::binary | std::ios::ate);
    const auto size = input.tellg();
    if (!input || size <= 0) {
        LOG_ERROR("coverage_settle: merged cov is missing or empty");
        return false;
    }
    merged->resize(static_cast<size_t>(size));
    input.seekg(0);
    input.read(merged->data(), static_cast<std::streamsize>(size));
    return static_cast<bool>(input);
}

bool RunSettle(const Args& args) {
    Connection conn(PQconnectdb(args.db_conn.c_str()), PQfinish);
    if (!conn || PQstatus(conn.get()) != CONNECTION_OK) {
        LOG_ERROR("coverage_settle: postgres connect failed: "
                  << (conn ? PQerrorMessage(conn.get()) : "PQconnectdb returned null"));
        return false;
    }
    // Take the session lock before opening the snapshot; PQfinish releases it
    // on every exit path. Concurrent processing of this run fails explicitly.
    auto lock = Query(conn.get(),
        "SELECT pg_try_advisory_lock(hashtextextended('coverage_settle:' || $1, 0))", args.run_id);
    if (!lock || PQntuples(lock.get()) != 1 || Cell(lock.get(), 0, 0) != "t") {
        LOG_ERROR("coverage_settle: another post-processor is settling this run");
        return false;
    }
    // Schema setup uses short transactions. Never retain the shared schema
    // lock while an external merge tool runs: execution clients may start then.
    if (!common::EnsurePluginResultSchema(conn.get()) || !EnsureDerivedSchema(conn.get())) return false;
    Transaction transaction{conn.get()};
    if (!transaction.begin()) return false;
    std::unordered_map<std::string, std::set<std::uint64_t>> file_points;
    if (!LoadCompleteRun(conn.get(), args, &file_points)) return false;
    std::set<std::uint64_t> cumulative;
    const auto selected = coverage_plugin::SelectGreedyMinSet(file_points, &cumulative);
    if (!common::PgExec(conn.get(), "DELETE FROM coverage_selected WHERE run_id=$1",
                        {{args.run_id, false}})) return false;
    for (const auto& file : selected) {
        if (!common::PgExec(conn.get(),
            "INSERT INTO coverage_selected(run_id,file) VALUES($1,$2)",
            {{args.run_id, false}, {file, false}})) return false;
    }
    std::string merged;
    std::int64_t covered = 0, total = 0;
    const bool merge = args.merge_cov && !selected.empty();
    if (merge && !MergeSelected(conn.get(), args, selected.size(), &merged, &covered, &total)) {
        return false;
    }
    // Selection-only reruns invalidate the old merged report; raw binaries stay.
    if (!common::PgExec(conn.get(), "DELETE FROM coverage_cumulative_cov WHERE run_id=$1",
                        {{args.run_id, false}})) return false;
    if (merge && !common::PgExec(conn.get(),
        "INSERT INTO coverage_cumulative_cov(run_id,cov) VALUES($1,$2)",
        {{args.run_id, false}, {merged, true}})) return false;
    const char* summary =
        "INSERT INTO coverage_run_summary(run_id,total_files,selected_files,cumulative_points,"
        "covered_lines,total_lines,percent,finished_at) "
        "VALUES($1,$2,$3,$4,NULLIF($5,'')::bigint,NULLIF($6,'')::bigint,"
        "NULLIF($7,'')::double precision,now()) ON CONFLICT(run_id) DO UPDATE SET "
        "total_files=EXCLUDED.total_files,selected_files=EXCLUDED.selected_files,"
        "cumulative_points=EXCLUDED.cumulative_points,covered_lines=EXCLUDED.covered_lines,"
        "total_lines=EXCLUDED.total_lines,percent=EXCLUDED.percent,finished_at=now()";
    if (!common::PgExec(conn.get(), summary,
        {{args.run_id, false}, {std::to_string(file_points.size()), false},
         {std::to_string(selected.size()), false}, {std::to_string(cumulative.size()), false},
         {merge ? std::to_string(covered) : "", false},
         {merge ? std::to_string(total) : "", false},
         {merge ? std::to_string(static_cast<double>(covered) / total) : "", false}}) ||
        !transaction.commit()) return false;
    LOG_INFO("coverage_settle: run " << args.run_id << " selected " << selected.size() << "/"
              << file_points.size() << " files, covered points=" << cumulative.size()
              << " (greedy cover; global minimum not guaranteed)");
    return true;
}
#else
bool RunSettle(const Args&) {
    LOG_ERROR("coverage_settle: this tool requires Windows/libpq");
    return false;
}
#endif
}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!ParseArgs(argc, argv, &args)) {
        std::fprintf(stderr, "usage: coverage_settle --run <run_id> --db <conninfo> "
                     "[--merge-cov] [--opencppcoverage <exe>] [--workdir <parent-dir>]\n");
        return 2;
    }
    try {
        return RunSettle(args) ? 0 : 1;
    } catch (const std::exception& error) {
        LOG_ERROR("coverage_settle: " << error.what());
        return 1;
    }
}
