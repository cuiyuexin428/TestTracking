#include "coverage/CoverageStore.h"

#include "common/Logger.h"
#include "common/PgUtil.h"
#include "common/Util.h"

#include "coverage/CoverageUtil.h"

#include <json.hpp>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <utility>

#ifdef _WIN32
#include <libpq-fe.h>
#endif

namespace coverage_plugin {

namespace {

std::string RunDir(const std::string& store_dir, const std::string& run_id) {
    return (std::filesystem::path(store_dir) / run_id).string();
}

std::string PointFileName(const std::string& file) {
    return std::to_string(common::Fnv1a64(file)) + ".json";
}

std::string CovFileName(const std::string& file) {
    return std::to_string(common::Fnv1a64(file)) + ".cov";
}

class FileCoverageStore : public CoverageResultStore, public CoverageCheckpointStore {
public:
    bool init(const std::string& config) override {
        const nlohmann::json cfg = nlohmann::json::parse(config);
        store_dir_ = cfg.value("store_dir", std::string());
        if (store_dir_.empty()) {
            LOG_ERROR("coverage store: store_dir is required for file backend");
            return false;
        }
        std::error_code ec;
        std::filesystem::create_directories(store_dir_, ec);
        if (ec) {
            LOG_ERROR("coverage store: cannot create store_dir " << store_dir_ << ": "
                                                                 << ec.message());
            return false;
        }
        return true;
    }

    bool saveFileCoverage(const std::string& run_id, const std::string& file,
                          const std::vector<std::uint64_t>& points) override {
        nlohmann::json json;
        json["file"] = file;
        for (std::uint64_t point : points) {
            json["points"].push_back(point);
        }
        const std::filesystem::path dir(RunDir(store_dir_, run_id));
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            LOG_ERROR("coverage store: cannot create run dir " << dir << ": " << ec.message());
            return false;
        }
        std::ofstream out((dir / PointFileName(file)).string());
        if (!out.is_open()) {
            LOG_ERROR("coverage store: cannot open point file for " << file);
            return false;
        }
        out << json.dump();
        out.flush();
        if (!out.good()) {
            LOG_ERROR("coverage store: write point file failed for " << file);
            return false;
        }
        return true;
    }

    bool saveFileCov(const std::string& run_id, const std::string& file,
                     const std::string& cov_data, std::int64_t size_bytes) override {
        const std::filesystem::path dir(RunDir(store_dir_, run_id));
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            LOG_ERROR("coverage store: cannot create run dir " << dir << ": " << ec.message());
            return false;
        }
        std::ofstream out((dir / CovFileName(file)).string(), std::ios::binary);
        if (!out.is_open()) {
            LOG_ERROR("coverage store: cannot open cov file for " << file);
            return false;
        }
        out.write(cov_data.data(), static_cast<std::streamsize>(size_bytes));
        out.flush();
        if (!out.good()) {
            LOG_ERROR("coverage store: write cov file failed for " << file);
            return false;
        }
        return true;
    }

    bool loadFileCoverage(const std::string& run_id, const std::string& file,
                          std::set<std::uint64_t>* points) override {
        if (points == nullptr) {
            return false;
        }
        points->clear();
        const std::filesystem::path path =
            std::filesystem::path(RunDir(store_dir_, run_id)) / PointFileName(file);
        std::ifstream in(path.string());
        if (!in.is_open()) {
            return false;
        }
        const nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
        if (json.is_discarded() || !json.contains("points")) {
            return false;
        }
        for (const nlohmann::json& point : json["points"]) {
            points->insert(point.get<std::uint64_t>());
        }
        return true;
    }

    bool saveCheckpoint(const std::string& run_id, const std::string& blob) override {
        const std::filesystem::path dir(store_dir_);
        std::error_code ec;
        std::filesystem::create_directories(dir / run_id, ec);
        if (ec) {
            LOG_ERROR("coverage store: cannot create run dir: " << ec.message());
            return false;
        }
        std::ofstream out((dir / run_id / "checkpoint.bin").string(), std::ios::binary);
        if (!out.is_open()) {
            LOG_ERROR("coverage store: cannot open checkpoint.bin");
            return false;
        }
        out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
        out.flush();
        if (!out.good()) {
            LOG_ERROR("coverage store: write checkpoint.bin failed");
            return false;
        }
        return true;
    }

    bool loadCheckpoint(const std::string& run_id, std::string* blob) override {
        if (blob == nullptr) {
            return false;
        }
        std::ifstream in((std::filesystem::path(store_dir_) / run_id / "checkpoint.bin").string(),
                         std::ios::binary | std::ios::ate);
        if (!in.is_open()) {
            return false;
        }
        const std::streamsize size = in.tellg();
        blob->assign(static_cast<size_t>(size), '\0');
        in.seekg(0);
        if (size > 0) {
            in.read(&(*blob)[0], size);
        }
        return true;
    }

private:
    std::string store_dir_;
};

#ifdef _WIN32

class PostgresCoverageStore : public CoverageResultStore, public CoverageCheckpointStore {
public:
    ~PostgresCoverageStore() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (conn_ != nullptr) {
            PQfinish(conn_);
            conn_ = nullptr;
        }
    }

    bool init(const std::string& config) override {
        const nlohmann::json cfg = nlohmann::json::parse(config);
        const std::string conninfo = cfg.value("db_conn", std::string());
        if (conninfo.empty()) {
            LOG_ERROR("coverage store: postgres requires db_conn");
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        conn_ = PQconnectdb(conninfo.c_str());
        if (conn_ == nullptr || PQstatus(conn_) != CONNECTION_OK) {
            LOG_ERROR("coverage store: postgres connect failed: "
                      << (conn_ ? PQerrorMessage(conn_) : "PQconnectdb returned null"));
            if (conn_ != nullptr) {
                PQfinish(conn_);
                conn_ = nullptr;
            }
            return false;
        }
        return EnsureSchema();
    }

    bool saveFileCoverage(const std::string& run_id, const std::string& file,
                          const std::vector<std::uint64_t>& points) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return common::PgExec(conn_,
                      "INSERT INTO coverage_file_points(run_id, file, points) "
                      "VALUES($1,$2,$3) "
                      "ON CONFLICT (run_id, file) DO UPDATE SET points=EXCLUDED.points",
                      {{run_id, false}, {file, false}, {PackPoints(points), true}});
    }

    bool saveFileCov(const std::string& run_id, const std::string& file,
                     const std::string& cov_data, std::int64_t size_bytes) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return common::PgExec(conn_,
                      "INSERT INTO coverage_cov_files(run_id, file, cov, size_bytes) "
                      "VALUES($1,$2,$3,$4) "
                      "ON CONFLICT (run_id, file) DO UPDATE "
                      "SET cov=EXCLUDED.cov, size_bytes=EXCLUDED.size_bytes",
                      {{run_id, false}, {file, false}, {cov_data, true},
                       {std::to_string(size_bytes), false}});
    }

    bool loadFileCoverage(const std::string& run_id, const std::string& file,
                          std::set<std::uint64_t>* points) override {
        if (points == nullptr) {
            return false;
        }
        points->clear();
        std::lock_guard<std::mutex> lock(mutex_);
        const char* sql = "SELECT points FROM coverage_file_points WHERE run_id=$1 AND file=$2";
        const std::vector<common::PgParam> params = {{run_id, false}, {file, false}};
        std::vector<const char*> values;
        std::vector<int> lengths;
        std::vector<int> formats;
        for (const common::PgParam& p : params) {
            values.push_back(p.value.data());
            lengths.push_back(static_cast<int>(p.value.size()));
            formats.push_back(0);
        }
        PGresult* result =
            PQexecParams(conn_, sql, static_cast<int>(params.size()), nullptr, values.data(),
                         lengths.data(), formats.data(), 0);
        if (result == nullptr || PQresultStatus(result) != PGRES_TUPLES_OK) {
            LOG_ERROR("coverage store: loadFileCoverage failed: "
                      << (result ? PQresultErrorMessage(result) : PQerrorMessage(conn_)));
            PQclear(result);
            return false;
        }
        const bool found = PQntuples(result) > 0;
        if (found) {
            const std::string blob =
                common::DecodeByteaText(PQgetvalue(result, 0, 0), PQgetlength(result, 0, 0));
            for (std::uint64_t p : UnpackPoints(blob)) {
                points->insert(p);
            }
        }
        PQclear(result);
        return found;
    }

    bool saveCheckpoint(const std::string& run_id, const std::string& blob) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return common::PgExec(conn_,
                      "INSERT INTO coverage_checkpoints(run_id, blob) VALUES($1,$2) "
                      "ON CONFLICT (run_id) DO UPDATE SET blob=EXCLUDED.blob, "
                      "updated_at=now()",
                      {{run_id, false}, {blob, true}});
    }

    bool loadCheckpoint(const std::string& run_id, std::string* blob) override {
        if (blob == nullptr) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        const char* sql = "SELECT blob FROM coverage_checkpoints WHERE run_id=$1";
        const std::vector<common::PgParam> params = {{run_id, false}};
        std::vector<const char*> values;
        std::vector<int> lengths;
        std::vector<int> formats;
        for (const common::PgParam& p : params) {
            values.push_back(p.value.data());
            lengths.push_back(static_cast<int>(p.value.size()));
            formats.push_back(0);
        }
        PGresult* result =
            PQexecParams(conn_, sql, static_cast<int>(params.size()), nullptr, values.data(),
                         lengths.data(), formats.data(), 0);
        bool ok = false;
        if (result != nullptr && PQresultStatus(result) == PGRES_TUPLES_OK &&
            PQntuples(result) > 0) {
            *blob = common::DecodeByteaText(PQgetvalue(result, 0, 0), PQgetlength(result, 0, 0));
            ok = true;
        } else {
            LOG_ERROR("coverage store: loadCheckpoint failed: "
                      << (result ? PQresultErrorMessage(result) : PQerrorMessage(conn_)));
        }
        PQclear(result);
        return ok;
    }

private:
    bool EnsureSchema() {
        const char* ddl =
            "CREATE TABLE IF NOT EXISTS coverage_file_points ("
            " run_id text NOT NULL, file text NOT NULL,"
            " points bytea NOT NULL, PRIMARY KEY (run_id, file));"
            "CREATE TABLE IF NOT EXISTS coverage_cov_files ("
            " run_id text NOT NULL, file text NOT NULL,"
            " cov bytea, size_bytes bigint, PRIMARY KEY (run_id, file));"
            "CREATE TABLE IF NOT EXISTS coverage_selected ("
            " run_id text NOT NULL, file text NOT NULL,"
            " selected_at timestamptz DEFAULT now(), PRIMARY KEY (run_id, file));"
            "CREATE TABLE IF NOT EXISTS coverage_run_summary ("
            " run_id text PRIMARY KEY, total_files int, selected_files int,"
            " cumulative_points bigint, covered_lines bigint, total_lines bigint,"
            " percent double precision, finished_at timestamptz);"
            "CREATE TABLE IF NOT EXISTS coverage_checkpoints ("
            " run_id text PRIMARY KEY, blob bytea NOT NULL,"
            " updated_at timestamptz DEFAULT now());"
            "CREATE TABLE IF NOT EXISTS coverage_cumulative_cov ("
            " run_id text PRIMARY KEY, cov bytea,"
            " updated_at timestamptz DEFAULT now());";
        PGresult* result = PQexec(conn_, ddl);
        bool schema_ok = true;
        if (result == nullptr || PQresultStatus(result) != PGRES_COMMAND_OK) {
            schema_ok = false;
            LOG_ERROR("coverage store: schema init failed: "
                      << (result ? PQresultErrorMessage(result) : PQerrorMessage(conn_)));
        }
        PQclear(result);
        return schema_ok;
    }

    PGconn* conn_ = nullptr;
    mutable std::mutex mutex_;
};

#endif  // _WIN32

}  // namespace

std::unique_ptr<CoverageResultStore> CreateCoverageResultStore(const std::string& config) {
    const nlohmann::json cfg = nlohmann::json::parse(config);
#ifdef _WIN32
    if (cfg.value("store_type", std::string("file")) == "postgres") {
        return std::make_unique<PostgresCoverageStore>();
    }
#else
    (void)cfg;
#endif
    return std::make_unique<FileCoverageStore>();
}

std::unique_ptr<CoverageCheckpointStore> CreateCoverageCheckpointStore(const std::string& config) {
    const nlohmann::json cfg = nlohmann::json::parse(config);
#ifdef _WIN32
    if (cfg.value("store_type", std::string("file")) == "postgres") {
        return std::make_unique<PostgresCoverageStore>();
    }
#else
    (void)cfg;
#endif
    return std::make_unique<FileCoverageStore>();
}

}  // namespace coverage_plugin
