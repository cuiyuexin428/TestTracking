#include "coverage/CoverageStore.h"

#include "common/Logger.h"

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

std::string WaveDir(const std::string& store_dir, const std::string& run_id, int wave) {
    return (std::filesystem::path(store_dir) / run_id / ("wave-" + std::to_string(wave)))
        .string();
}

std::string PointFileName(const std::string& file) {
    return std::to_string(Fnv1a64(file)) + ".json";
}

std::string CovFileName(const std::string& file) {
    return std::to_string(Fnv1a64(file)) + ".cov";
}

std::string PackPoints(const std::vector<std::uint64_t>& points) {
    std::string out;
    out.reserve(points.size() * sizeof(std::uint64_t));
    for (std::uint64_t p : points) {
        for (int i = 0; i < 8; ++i) {
            out.push_back(static_cast<char>((p >> (8 * i)) & 0xFF));
        }
    }
    return out;
}

std::vector<std::uint64_t> UnpackPoints(const std::string& data) {
    std::vector<std::uint64_t> out;
    out.reserve(data.size() / sizeof(std::uint64_t));
    for (size_t i = 0; i + 8 <= data.size(); i += 8) {
        std::uint64_t p = 0;
        for (int j = 0; j < 8; ++j) {
            p |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[i + j])) << (8 * j);
        }
        out.push_back(p);
    }
    return out;
}

class FileCoverageStore : public CoverageStore {
public:
    bool init(const std::string& config) override {
        const nlohmann::json cfg = nlohmann::json::parse(config);
        store_dir_ = cfg.value("store_dir", std::string());
        return !store_dir_.empty();
    }

    void saveFileCoverage(const std::string& run_id, int wave, const std::string& file,
                          const std::vector<std::uint64_t>& points) override {
        nlohmann::json json;
        json["file"] = file;
        for (std::uint64_t point : points) {
            json["points"].push_back(point);
        }

        const std::filesystem::path dir(WaveDir(store_dir_, run_id, wave));
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::ofstream out((dir / PointFileName(file)).string());
        if (out.is_open()) {
            out << json.dump();
        }
    }

    void saveFileCov(const std::string& run_id, int wave, const std::string& file,
                     const std::string& cov_data, std::int64_t size_bytes) override {
        const std::filesystem::path dir(WaveDir(store_dir_, run_id, wave));
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::ofstream out((dir / CovFileName(file)).string(), std::ios::binary);
        if (out.is_open()) {
            out.write(cov_data.data(), static_cast<std::streamsize>(size_bytes));
        }
    }

    std::unordered_map<std::string, std::set<std::uint64_t>> loadWave(
        const std::string& run_id, int wave) override {
        std::unordered_map<std::string, std::set<std::uint64_t>> file_points;
        const std::filesystem::path dir(WaveDir(store_dir_, run_id, wave));
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) {
                break;
            }
            if (!entry.is_regular_file() || entry.path().extension() != ".json") {
                continue;
            }
            std::ifstream in(entry.path().string());
            if (!in.is_open()) {
                continue;
            }
            const nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
            if (json.is_discarded() || !json.contains("file") || !json.contains("points")) {
                continue;
            }
            std::set<std::uint64_t>& points = file_points[json["file"].get<std::string>()];
            for (const nlohmann::json& point : json["points"]) {
                points.insert(point.get<std::uint64_t>());
            }
        }
        return file_points;
    }

    void clearWave(const std::string& run_id, int wave) override {
        std::error_code ec;
        std::filesystem::remove_all(WaveDir(store_dir_, run_id, wave), ec);
    }

    void appendSelected(const std::string& run_id,
                        const std::vector<std::string>& files) override {
        const std::filesystem::path dir(store_dir_);
        std::error_code ec;
        std::filesystem::create_directories(dir / run_id, ec);
        std::ofstream out((dir / run_id / "selected.txt").string(), std::ios::app);
        for (const std::string& file : files) {
            out << file << "\n";
        }
    }

    void saveCumulative(const std::string& run_id, int wave,
                        const std::vector<std::uint64_t>& points) override {
        nlohmann::json json;
        for (std::uint64_t point : points) {
            json["points"].push_back(point);
        }
        const std::filesystem::path dir(store_dir_);
        std::error_code ec;
        std::filesystem::create_directories(dir / run_id, ec);
        std::ofstream out((dir / run_id / ("cumulative-" + std::to_string(wave) + ".json"))
                              .string());
        if (out.is_open()) {
            out << json.dump();
        }
    }

    void saveRunSummary(const std::string& run_id, int total_files, int selected_files,
                        std::int64_t cumulative_points) override {
        nlohmann::json json;
        json["total_files"] = total_files;
        json["selected_files"] = selected_files;
        json["cumulative_points"] = cumulative_points;
        const std::filesystem::path dir(store_dir_);
        std::error_code ec;
        std::filesystem::create_directories(dir / run_id, ec);
        std::ofstream out((dir / run_id / "summary.json").string());
        if (out.is_open()) {
            out << json.dump();
        }
    }

    void saveCheckpoint(const std::string& run_id, const std::string& blob) override {
        const std::filesystem::path dir(store_dir_);
        std::error_code ec;
        std::filesystem::create_directories(dir / run_id, ec);
        std::ofstream out((dir / run_id / "checkpoint.bin").string(), std::ios::binary);
        if (out.is_open()) {
            out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
        }
    }

    bool loadCheckpoint(const std::string& run_id, std::string* blob) override {
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

    std::vector<std::uint64_t> loadCumulative(const std::string& run_id, int wave) override {
        std::vector<std::uint64_t> out;
        if (wave < 0) {
            return out;
        }
        std::ifstream in((std::filesystem::path(store_dir_) / run_id /
                          ("cumulative-" + std::to_string(wave) + ".json"))
                             .string());
        if (!in.is_open()) {
            return out;
        }
        const nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
        if (json.is_discarded() || !json.contains("points")) {
            return out;
        }
        out.reserve(json["points"].size());
        for (const nlohmann::json& point : json["points"]) {
            out.push_back(point.get<std::uint64_t>());
        }
        return out;
    }

private:
    std::string store_dir_;
};

#ifdef _WIN32

struct PgParam {
    std::string value;
    bool binary = false;
};

bool PgExec(PGconn* conn, const char* sql, const std::vector<PgParam>& params) {
    if (conn == nullptr) {
        return false;
    }
    std::vector<const char*> values;
    std::vector<int> lengths;
    std::vector<int> formats;
    values.reserve(params.size());
    lengths.reserve(params.size());
    formats.reserve(params.size());
    for (const PgParam& p : params) {
        values.push_back(p.value.data());
        lengths.push_back(static_cast<int>(p.value.size()));
        formats.push_back(p.binary ? 1 : 0);
    }
    PGresult* result =
        PQexecParams(conn, sql, static_cast<int>(params.size()), nullptr, values.data(),
                     lengths.data(), formats.data(), 0);
    const bool ok = result != nullptr && PQresultStatus(result) == PGRES_COMMAND_OK;
    if (!ok) {
        LOG_ERROR("coverage store: sql failed: " << (result ? PQresultErrorMessage(result)
                                                            : PQerrorMessage(conn)));
    }
    PQclear(result);
    return ok;
}

std::string DecodeByteaText(const char* text, int length) {
    // Server text format for bytea: \x<hex>.
    if (text == nullptr || length < 2 || text[0] != '\\' || text[1] != 'x') {
        return std::string();
    }
    const std::string hex(text + 2, static_cast<size_t>(length - 2));
    std::string out;
    out.reserve(hex.size() / 2);
    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<char>((hexval(hex[i]) << 4) | hexval(hex[i + 1])));
    }
    return out;
}

class PostgresCoverageStore : public CoverageStore {
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
        EnsureSchema();
        return true;
    }

    void saveFileCoverage(const std::string& run_id, int wave, const std::string& file,
                          const std::vector<std::uint64_t>& points) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PgExec(conn_,
               "INSERT INTO coverage_wave_points(run_id, wave_id, file, points) "
               "VALUES($1,$2,$3,$4) "
               "ON CONFLICT (run_id, wave_id, file) DO UPDATE SET points=EXCLUDED.points",
               {{run_id, false}, {std::to_string(wave), false}, {file, false},
                {PackPoints(points), true}});
    }

    void saveFileCov(const std::string& run_id, int wave, const std::string& file,
                     const std::string& cov_data, std::int64_t size_bytes) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PgExec(conn_,
               "INSERT INTO coverage_cov_files(run_id, wave_id, file, cov, size_bytes) "
               "VALUES($1,$2,$3,$4,$5) "
               "ON CONFLICT (run_id, wave_id, file) DO UPDATE "
               "SET cov=EXCLUDED.cov, size_bytes=EXCLUDED.size_bytes",
               {{run_id, false}, {std::to_string(wave), false}, {file, false},
                {cov_data, true}, {std::to_string(size_bytes), false}});
    }

    std::unordered_map<std::string, std::set<std::uint64_t>> loadWave(
        const std::string& run_id, int wave) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::unordered_map<std::string, std::set<std::uint64_t>> file_points;
        const char* sql = "SELECT file, points FROM coverage_wave_points "
                          "WHERE run_id=$1 AND wave_id=$2";
        const std::vector<PgParam> params = {{run_id, false}, {std::to_string(wave), false}};
        std::vector<const char*> values;
        std::vector<int> lengths;
        std::vector<int> formats;
        for (const PgParam& p : params) {
            values.push_back(p.value.data());
            lengths.push_back(static_cast<int>(p.value.size()));
            formats.push_back(0);
        }
        PGresult* result =
            PQexecParams(conn_, sql, static_cast<int>(params.size()), nullptr, values.data(),
                         lengths.data(), formats.data(), 0);
        if (result != nullptr && PQresultStatus(result) == PGRES_TUPLES_OK) {
            const int rows = PQntuples(result);
            for (int i = 0; i < rows; ++i) {
                const std::string file(PQgetvalue(result, i, 0));
                const std::string blob = DecodeByteaText(PQgetvalue(result, i, 1),
                                                         PQgetlength(result, i, 1));
                std::set<std::uint64_t>& points = file_points[file];
                for (std::uint64_t p : UnpackPoints(blob)) {
                    points.insert(p);
                }
            }
        } else {
            LOG_ERROR("coverage store: loadWave failed: "
                      << (result ? PQresultErrorMessage(result) : PQerrorMessage(conn_)));
        }
        PQclear(result);
        return file_points;
    }

    void clearWave(const std::string& run_id, int wave) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PgExec(conn_, "DELETE FROM coverage_wave_points WHERE run_id=$1 AND wave_id=$2",
               {{run_id, false}, {std::to_string(wave), false}});
    }

    void appendSelected(const std::string& run_id,
                        const std::vector<std::string>& files) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const std::string& file : files) {
            PgExec(conn_,
                   "INSERT INTO coverage_selected(run_id, file) VALUES($1,$2) "
                   "ON CONFLICT (run_id, file) DO NOTHING",
                   {{run_id, false}, {file, false}});
        }
    }

    void saveCumulative(const std::string& run_id, int wave,
                        const std::vector<std::uint64_t>& points) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PgExec(conn_,
               "INSERT INTO coverage_cumulative(run_id, wave_id, points) VALUES($1,$2,$3) "
               "ON CONFLICT (run_id, wave_id) DO UPDATE SET points=EXCLUDED.points",
               {{run_id, false}, {std::to_string(wave), false}, {PackPoints(points), true}});
    }

    void saveRunSummary(const std::string& run_id, int total_files, int selected_files,
                        std::int64_t cumulative_points) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PgExec(conn_,
               "INSERT INTO coverage_run_summary(run_id, total_files, selected_files, "
               "cumulative_points, finished_at) VALUES($1,$2,$3,$4,now()) "
               "ON CONFLICT (run_id) DO UPDATE SET total_files=EXCLUDED.total_files, "
               "selected_files=EXCLUDED.selected_files, "
               "cumulative_points=EXCLUDED.cumulative_points, "
               "finished_at=EXCLUDED.finished_at",
               {{run_id, false}, {std::to_string(total_files), false},
                {std::to_string(selected_files), false},
                {std::to_string(cumulative_points), false}});
    }

    void saveCheckpoint(const std::string& run_id, const std::string& blob) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PgExec(conn_,
               "INSERT INTO coverage_checkpoints(run_id, blob) VALUES($1,$2) "
               "ON CONFLICT (run_id) DO UPDATE SET blob=EXCLUDED.blob, updated_at=now()",
               {{run_id, false}, {blob, true}});
    }

    bool loadCheckpoint(const std::string& run_id, std::string* blob) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const char* sql = "SELECT blob FROM coverage_checkpoints WHERE run_id=$1";
        const std::vector<PgParam> params = {{run_id, false}};
        std::vector<const char*> values;
        std::vector<int> lengths;
        std::vector<int> formats;
        for (const PgParam& p : params) {
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
            *blob = DecodeByteaText(PQgetvalue(result, 0, 0), PQgetlength(result, 0, 0));
            ok = true;
        } else {
            LOG_ERROR("coverage store: loadCheckpoint failed: "
                      << (result ? PQresultErrorMessage(result) : PQerrorMessage(conn_)));
        }
        PQclear(result);
        return ok;
    }

    std::vector<std::uint64_t> loadCumulative(const std::string& run_id, int wave) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::uint64_t> out;
        if (wave < 0) {
            return out;
        }
        const char* sql =
            "SELECT points FROM coverage_cumulative WHERE run_id=$1 AND wave_id=$2";
        const std::vector<PgParam> params = {{run_id, false}, {std::to_string(wave), false}};
        std::vector<const char*> values;
        std::vector<int> lengths;
        std::vector<int> formats;
        for (const PgParam& p : params) {
            values.push_back(p.value.data());
            lengths.push_back(static_cast<int>(p.value.size()));
            formats.push_back(0);
        }
        PGresult* result =
            PQexecParams(conn_, sql, static_cast<int>(params.size()), nullptr, values.data(),
                         lengths.data(), formats.data(), 0);
        if (result != nullptr && PQresultStatus(result) == PGRES_TUPLES_OK &&
            PQntuples(result) > 0) {
            const std::string blob =
                DecodeByteaText(PQgetvalue(result, 0, 0), PQgetlength(result, 0, 0));
            for (std::uint64_t p : UnpackPoints(blob)) {
                out.push_back(p);
            }
        } else {
            LOG_ERROR("coverage store: loadCumulative failed: "
                      << (result ? PQresultErrorMessage(result) : PQerrorMessage(conn_)));
        }
        PQclear(result);
        return out;
    }

private:
    void EnsureSchema() {
        const char* ddl =
            "CREATE TABLE IF NOT EXISTS coverage_wave_points ("
            " run_id text NOT NULL, wave_id int NOT NULL, file text NOT NULL,"
            " points bytea NOT NULL, PRIMARY KEY (run_id, wave_id, file));"
            "CREATE TABLE IF NOT EXISTS coverage_cov_files ("
            " run_id text NOT NULL, wave_id int NOT NULL, file text NOT NULL,"
            " cov bytea, size_bytes bigint, PRIMARY KEY (run_id, wave_id, file));"
            "CREATE TABLE IF NOT EXISTS coverage_cumulative ("
            " run_id text NOT NULL, wave_id int NOT NULL, points bytea,"
            " PRIMARY KEY (run_id, wave_id));"
            "CREATE TABLE IF NOT EXISTS coverage_selected ("
            " run_id text NOT NULL, file text NOT NULL,"
            " selected_at timestamptz DEFAULT now(), PRIMARY KEY (run_id, file));"
            "CREATE TABLE IF NOT EXISTS coverage_run_summary ("
            " run_id text PRIMARY KEY, total_files int, selected_files int,"
            " cumulative_points bigint, finished_at timestamptz);"
            "CREATE TABLE IF NOT EXISTS coverage_checkpoints ("
            " run_id text PRIMARY KEY, blob bytea NOT NULL,"
            " updated_at timestamptz DEFAULT now());";
        PGresult* result = PQexec(conn_, ddl);
        if (result == nullptr || PQresultStatus(result) != PGRES_COMMAND_OK) {
            LOG_ERROR("coverage store: schema init failed: "
                      << (result ? PQresultErrorMessage(result) : PQerrorMessage(conn_)));
        }
        PQclear(result);
    }

    PGconn* conn_ = nullptr;
    mutable std::mutex mutex_;
};

#endif  // _WIN32

}  // namespace

std::unique_ptr<CoverageStore> CreateCoverageStore(const std::string& config) {
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
