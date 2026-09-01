// Standalone coverage post-processor.
//
// After a run finishes dispatching (the scheduler only hands out tasks and
// clients write raw point sets / cov binaries into the store), this tool
// consumes the retained raw rows for a run and produces the derived outputs:
//   * the minimal stp file set (global greedy over all files' point sets)
//     written into coverage_selected;
//   * an optional cumulative cov merge (OpenCppCoverage) of the selected files
//     plus the covered/total/percent totals into coverage_run_summary and the
//     merged binary into coverage_cumulative_cov.
//
// Usage:
//   settle_coverage --run <run_id> --db <conninfo>
//                  [--opencppcoverage <exe>] [--workdir <dir>]
//
// The post-processor only reads coverage_file_points / coverage_cov_files and
// writes the derived tables; raw rows are retained (never deleted here).

#include "coverage/CoverageUtil.h"

#include "common/Logger.h"
#include "common/PgUtil.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <libpq-fe.h>
#endif

namespace {

struct Args {
    std::string run_id;
    std::string db_conn;
    std::string opencppcoverage = "OpenCppCoverage.exe";
    std::string workdir;
};

bool ParseArgs(int argc, char** argv, Args* out) {
    if (out == nullptr) {
        return false;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&](std::string* into) -> bool {
            if (i + 1 >= argc) {
                return false;
            }
            *into = argv[++i];
            return true;
        };
        if (arg == "--run") {
            if (!next(&out->run_id)) return false;
        } else if (arg == "--db") {
            if (!next(&out->db_conn)) return false;
        } else if (arg == "--opencppcoverage") {
            if (!next(&out->opencppcoverage)) return false;
        } else if (arg == "--workdir") {
            if (!next(&out->workdir)) return false;
        } else {
            LOG_ERROR("settle: unknown arg " << arg);
            return false;
        }
    }
    return !out->run_id.empty() && !out->db_conn.empty();
}

#ifdef _WIN32

// Runs a parameterized query with a single text $1 argument and hands each
// result row to handle. Sql must contain exactly one $1 placeholder.
template <typename F>
bool QueryText(PGconn* conn, const char* sql, const std::string& a, F handle) {
    const char* values[] = {a.c_str()};
    const int lengths[] = {static_cast<int>(a.size())};
    const int formats[] = {0};
    PGresult* result =
        PQexecParams(conn, sql, 1, nullptr, values, lengths, formats, 0);
    if (result == nullptr || PQresultStatus(result) != PGRES_TUPLES_OK) {
        LOG_ERROR("settle: query failed: "
                  << (result ? PQresultErrorMessage(result) : PQerrorMessage(conn)));
        PQclear(result);
        return false;
    }
    const int rows = PQntuples(result);
    for (int i = 0; i < rows; ++i) {
        handle(result, i);
    }
    PQclear(result);
    return true;
}

bool CommandOk(PGconn* conn, PGresult* result, const char* what) {
    if (result == nullptr || PQresultStatus(result) != PGRES_COMMAND_OK) {
        LOG_ERROR("settle: " << what << " failed: "
                             << (result ? PQresultErrorMessage(result) : PQerrorMessage(conn)));
        PQclear(result);
        return false;
    }
    PQclear(result);
    return true;
}

// Insert-or-ignore one selected row. run_id/file are text params.
bool InsertSelected(PGconn* conn, const std::string& run_id, const std::string& file) {
    const char* sql = "INSERT INTO coverage_selected(run_id, file) VALUES($1,$2) "
                      "ON CONFLICT (run_id, file) DO NOTHING";
    const char* values[] = {run_id.c_str(), file.c_str()};
    const int lengths[] = {static_cast<int>(run_id.size()), static_cast<int>(file.size())};
    const int formats[] = {0, 0};
    PGresult* result =
        PQexecParams(conn, sql, 2, nullptr, values, lengths, formats, 0);
    return CommandOk(conn, result, "insert selected");
}

bool RunSettle(const Args& args) {
    PGconn* conn = PQconnectdb(args.db_conn.c_str());
    if (conn == nullptr || PQstatus(conn) != CONNECTION_OK) {
        LOG_ERROR("settle: postgres connect failed: "
                  << (conn ? PQerrorMessage(conn) : "PQconnectdb returned null"));
        if (conn != nullptr) {
            PQfinish(conn);
        }
        return false;
    }

    // Load all raw point sets for the run (global greedy source).
    std::unordered_map<std::string, std::set<std::uint64_t>> file_points;
    const bool points_ok = QueryText(
        conn, "SELECT file, points FROM coverage_file_points WHERE run_id=$1", args.run_id,
        [&](PGresult* result, int row) {
            const std::string file(PQgetvalue(result, row, 0));
            const std::string blob =
                common::DecodeByteaText(PQgetvalue(result, row, 1), PQgetlength(result, row, 1));
            for (std::uint64_t p : coverage_plugin::UnpackPoints(blob)) {
                file_points[file].insert(p);
            }
        });
    if (!points_ok) {
        PQfinish(conn);
        return false;
    }
    if (file_points.empty()) {
        LOG_ERROR("settle: no coverage data for run " << args.run_id);
        PQfinish(conn);
        return false;
    }
    const int total_files = static_cast<int>(file_points.size());

    // Global greedy minimal set.
    std::set<std::uint64_t> cumulative;
    const std::vector<std::string> selected =
        coverage_plugin::SelectGreedyMinSet(file_points, &cumulative);

    // Replace this run's selected rows (idempotent re-run).
    {
        const char* del = "DELETE FROM coverage_selected WHERE run_id=$1";
        const char* values[] = {args.run_id.c_str()};
        const int lengths[] = {static_cast<int>(args.run_id.size())};
        const int formats[] = {0};
        PGresult* r = PQexecParams(conn, del, 1, nullptr, values, lengths, formats, 0);
        if (!CommandOk(conn, r, "clear selected")) {
            PQfinish(conn);
            return false;
        }
    }
    for (const std::string& file : selected) {
        if (!InsertSelected(conn, args.run_id, file)) {
            PQfinish(conn);
            return false;
        }
    }

    // Load cov binaries for the run so the selected files can be merged.
    std::unordered_map<std::string, std::string> selected_covs;
    const bool cov_ok = QueryText(
        conn, "SELECT file, cov FROM coverage_cov_files WHERE run_id=$1", args.run_id,
        [&](PGresult* result, int row) {
            const std::string file(PQgetvalue(result, row, 0));
            const std::string cov = common::DecodeByteaText(PQgetvalue(result, row, 1),
                                                    PQgetlength(result, row, 1));
            for (const std::string& s : selected) {
                if (s == file) {
                    selected_covs[file] = cov;
                    break;
                }
            }
        });
    if (!cov_ok) {
        LOG_WARN("settle: cov query failed; continuing without cov merge");
    }

    std::int64_t covered = 0;
    std::int64_t total = 0;
    double percent = 0.0;
    if (!selected_covs.empty()) {
        const std::filesystem::path work =
            args.workdir.empty()
                ? std::filesystem::temp_directory_path() / ("tt-settle-" + args.run_id)
                : std::filesystem::path(args.workdir);
        std::error_code ec;
        std::filesystem::remove_all(work, ec);
        std::filesystem::create_directories(work, ec);
        if (ec) {
            LOG_ERROR("settle: cannot create work dir: " << ec.message());
            PQfinish(conn);
            return false;
        }
        std::vector<std::string> cov_paths;
        cov_paths.reserve(selected_covs.size());
        for (const std::string& file : selected) {
            const auto it = selected_covs.find(file);
            if (it == selected_covs.end()) {
                continue;
            }
            const std::filesystem::path p =
                work / ("f-" + std::to_string(common::Fnv1a64(file)) + ".cov");
            std::ofstream out(p.string(), std::ios::binary);
            out.write(it->second.data(), static_cast<std::streamsize>(it->second.size()));
            out.flush();
            if (!out.good()) {
                LOG_ERROR("settle: cannot write cov " << p);
                PQfinish(conn);
                return false;
            }
            cov_paths.push_back(p.string());
        }
        const std::string out_cov = (work / "cumulative.cov").string();
        const std::string out_xml = (work / "summary.xml").string();
        if (cov_paths.empty() ||
            !coverage_plugin::MergeCoverageFiles(args.opencppcoverage, cov_paths, out_cov,
                                                 out_xml)) {
            LOG_ERROR("settle: cov merge failed");
            PQfinish(conn);
            return false;
        }
        if (!coverage_plugin::ParseCoverageTotals(out_xml, &covered, &total)) {
            LOG_ERROR("settle: cannot parse merged cobertura");
            PQfinish(conn);
            return false;
        }
        percent = total > 0 ? static_cast<double>(covered) / total : 0.0;
        // Store the merged cumulative cov (binary param).
        std::ifstream cin(out_cov, std::ios::binary | std::ios::ate);
        if (cin.is_open()) {
            const std::streamsize sz = cin.tellg();
            std::string data(static_cast<size_t>(sz), '\0');
            cin.seekg(0);
            cin.read(&data[0], sz);
            const char* sql =
                "INSERT INTO coverage_cumulative_cov(run_id, cov, updated_at) "
                "VALUES($1,$2,now()) ON CONFLICT (run_id) DO UPDATE SET "
                "cov=EXCLUDED.cov, updated_at=now()";
            const char* values[] = {args.run_id.c_str(), data.data()};
            const int lengths[] = {static_cast<int>(args.run_id.size()),
                                   static_cast<int>(data.size())};
            const int formats[] = {0, 1};
            PGresult* r =
                PQexecParams(conn, sql, 2, nullptr, values, lengths, formats, 0);
            if (!CommandOk(conn, r, "save cumulative cov")) {
                PQfinish(conn);
                return false;
            }
        }
    }

    // Write the run summary.
    {
        const char* sql =
            "INSERT INTO coverage_run_summary(run_id, total_files, selected_files, "
            "cumulative_points, covered_lines, total_lines, percent, finished_at) "
            "VALUES($1,$2,$3,$4,$5,$6,$7,now()) ON CONFLICT (run_id) DO UPDATE SET "
            "total_files=EXCLUDED.total_files, selected_files=EXCLUDED.selected_files, "
            "cumulative_points=EXCLUDED.cumulative_points, "
            "covered_lines=EXCLUDED.covered_lines, total_lines=EXCLUDED.total_lines, "
            "percent=EXCLUDED.percent, finished_at=now()";
        const std::string total_files_s = std::to_string(total_files);
        const std::string sel_s = std::to_string(selected.size());
        const std::string cum_s = std::to_string(cumulative.size());
        const std::string covered_s = std::to_string(covered);
        const std::string total_s = std::to_string(total);
        const std::string percent_s = std::to_string(percent);
        const char* values[] = {args.run_id.c_str(), total_files_s.c_str(), sel_s.c_str(),
                                cum_s.c_str(),         covered_s.c_str(), total_s.c_str(),
                                percent_s.c_str()};
        const int lengths[] = {static_cast<int>(args.run_id.size()),
                               static_cast<int>(total_files_s.size()),
                               static_cast<int>(sel_s.size()),
                               static_cast<int>(cum_s.size()),
                               static_cast<int>(covered_s.size()),
                               static_cast<int>(total_s.size()),
                               static_cast<int>(percent_s.size())};
        const int formats[] = {0, 0, 0, 0, 0, 0, 0};
        PGresult* r =
            PQexecParams(conn, sql, 7, nullptr, values, lengths, formats, 0);
        if (!CommandOk(conn, r, "write summary")) {
            PQfinish(conn);
            return false;
        }
    }

    PQfinish(conn);
    LOG_INFO("settle: run " << args.run_id << " selected " << selected.size() << "/"
                            << total_files << " files, cumulative points=" << cumulative.size());
    return true;
}

#else  // !_WIN32

bool RunSettle(const Args&) {
    LOG_ERROR("settle: this tool is only supported on Windows (requires libpq)");
    return false;
}

#endif

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!ParseArgs(argc, argv, &args)) {
        std::fprintf(stderr, "usage: settle_coverage --run <run_id> --db <conninfo> "
                             "[--opencppcoverage <exe>] [--workdir <dir>]\n");
        return 2;
    }
    return RunSettle(args) ? 0 : 1;
}
