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

    bool saveFileCoverage(const std::string& run_id, int wave, const std::string& file,
                          const std::vector<std::uint64_t>& points) override {
        nlohmann::json json;
        json["file"] = file;
        for (std::uint64_t point : points) {
            json["points"].push_back(point);
        }

        const std::filesystem::path dir(WaveDir(store_dir_, run_id, wave));
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            LOG_ERROR("coverage store: cannot create wave dir " << dir << ": " << ec.message());
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

    bool saveFileCov(const std::string& run_id, int wave, const std::string& file,
                     const std::string& cov_data, std::int64_t size_bytes) override {
        const std::filesystem::path dir(WaveDir(store_dir_, run_id, wave));
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            LOG_ERROR("coverage store: cannot create wave dir " << dir << ": " << ec.message());
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

    bool loadWave(const std::string& run_id, int wave,
                  std::unordered_map<std::string, std::set<std::uint64_t>>* file_points)
        override {
        if (file_points == nullptr) {
            return false;
        }
        file_points->clear();
        const std::filesystem::path dir(WaveDir(store_dir_, run_id, wave));
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec)) {
            return true;  // nothing staged for this wave
        }
        if (ec) {
            LOG_ERROR("coverage store: cannot stat wave dir " << dir << ": " << ec.message());
            return false;
        }
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) {
                break;
            }
            if (!entry.is_regular_file() || entry.path().extension() != ".json") {
                continue;
            }
            std::ifstream in(entry.path().string());
            if (!in.is_open()) {
                LOG_ERROR("coverage store: cannot open " << entry.path());
                return false;
            }
            const nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
            if (json.is_discarded() || !json.contains("file") || !json.contains("points")) {
                LOG_ERROR("coverage store: unparseable point file " << entry.path());
                return false;
            }
            std::set<std::uint64_t>& points =
                (*file_points)[json["file"].get<std::string>()];
            for (const nlohmann::json& point : json["points"]) {
                points.insert(point.get<std::uint64_t>());
            }
        }
        if (ec) {
            LOG_ERROR("coverage store: wave scan failed " << dir << ": " << ec.message());
            return false;
        }
        return true;
    }

    bool loadFileCoverage(const std::string& run_id, int wave, const std::string& file,
                          std::set<std::uint64_t>* points) override {
        if (points == nullptr) {
            return false;
        }
        points->clear();
        const std::filesystem::path path =
            std::filesystem::path(WaveDir(store_dir_, run_id, wave)) /
            PointFileName(file);
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

    bool clearWave(const std::string& run_id, int wave) override {
        std::error_code ec;
        std::filesystem::remove_all(WaveDir(store_dir_, run_id, wave), ec);
        if (ec) {
            LOG_ERROR("coverage store: clear wave failed: " << ec.message());
            return false;
        }
        return true;
    }

    bool appendSelected(const std::string& run_id,
                        const std::vector<std::string>& files) override {
        const std::filesystem::path dir(store_dir_);
        std::error_code ec;
        std::filesystem::create_directories(dir / run_id, ec);
        if (ec) {
            LOG_ERROR("coverage store: cannot create run dir: " << ec.message());
            return false;
        }
        const std::filesystem::path selected_path = dir / run_id / "selected.txt";
        std::set<std::string> existing;
        {
            std::ifstream in(selected_path.string());
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty()) {
                    existing.insert(line);
                }
            }
        }
        std::ofstream out(selected_path.string(), std::ios::app);
        if (!out.is_open()) {
            LOG_ERROR("coverage store: cannot open selected.txt");
            return false;
        }
        for (const std::string& file : files) {
            if (existing.count(file) > 0) {
                continue;
            }
            out << file << "\n";
            existing.insert(file);
        }
        out.flush();
        if (!out.good()) {
            LOG_ERROR("coverage store: write selected.txt failed");
            return false;
        }
        return true;
    }

    bool saveCumulative(const std::string& run_id, int wave,
                        const std::vector<std::uint64_t>& points) override {
        nlohmann::json json;
        for (std::uint64_t point : points) {
            json["points"].push_back(point);
        }
        const std::filesystem::path dir(store_dir_);
        std::error_code ec;
        std::filesystem::create_directories(dir / run_id, ec);
        if (ec) {
            LOG_ERROR("coverage store: cannot create run dir: " << ec.message());
            return false;
        }
        std::ofstream out(
            (dir / run_id / ("cumulative-" + std::to_string(wave) + ".json")).string());
        if (!out.is_open()) {
            LOG_ERROR("coverage store: cannot open cumulative file for wave " << wave);
            return false;
        }
        out << json.dump();
        out.flush();
        if (!out.good()) {
            LOG_ERROR("coverage store: write cumulative file failed for wave " << wave);
            return false;
        }
        return true;
    }

    bool saveRunSummary(const std::string& run_id, int total_files, int selected_files,
                        std::int64_t cumulative_points) override {
        nlohmann::json json;
        json["total_files"] = total_files;
        json["selected_files"] = selected_files;
        json["cumulative_points"] = cumulative_points;
        const std::filesystem::path dir(store_dir_);
        std::error_code ec;
        std::filesystem::create_directories(dir / run_id, ec);
        if (ec) {
            LOG_ERROR("coverage store: cannot create run dir: " << ec.message());
            return false;
        }
        std::ofstream out((dir / run_id / "summary.json").string());
        if (!out.is_open()) {
            LOG_ERROR("coverage store: cannot open summary.json");
            return false;
        }
        out << json.dump();
        out.flush();
        if (!out.good()) {
            LOG_ERROR("coverage store: write summary.json failed");
            return false;
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

    bool loadCumulative(const std::string& run_id, int wave,
                        std::vector<std::uint64_t>* out) override {
        if (out == nullptr) {
            return false;
        }
        out->clear();
        if (wave < 0) {
            return true;
        }
        std::ifstream in((std::filesystem::path(store_dir_) / run_id /
                          ("cumulative-" + std::to_string(wave) + ".json"))
                             .string());
        if (!in.is_open()) {
            LOG_ERROR("coverage store: cumulative file missing for wave " << wave);
            return false;
        }
        const nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
        if (json.is_discarded() || !json.contains("points")) {
            LOG_ERROR("coverage store: unparseable cumulative file for wave " << wave);
            return false;
        }
        out->reserve(json["points"].size());
        for (const nlohmann::json& point : json["points"]) {
            out->push_back(point.get<std::uint64_t>());
        }
        return true;
    }

    bool exportWaveCovs(const std::string& run_id, int wave, const std::string& dir,
                        std::vector<std::string>& paths) override {
        paths.clear();
        const std::filesystem::path wave_dir(WaveDir(store_dir_, run_id, wave));
        std::error_code ec;
        if (!std::filesystem::exists(wave_dir, ec)) {
            return true;  // no covs staged for this wave
        }
        if (ec) {
            LOG_ERROR("coverage store: cannot stat wave dir " << wave_dir << ": "
                                                              << ec.message());
            return false;
        }
        for (const auto& entry : std::filesystem::directory_iterator(wave_dir, ec)) {
            if (ec) {
                break;
            }
            if (entry.is_regular_file() && entry.path().extension() == ".cov") {
                paths.push_back(entry.path().string());
            }
        }
        if (ec) {
            LOG_ERROR("coverage store: cov scan failed " << wave_dir << ": " << ec.message());
            return false;
        }
        return true;
    }

    bool exportCumulativeCov(const std::string& run_id, const std::string& dir,
                             std::string& path) override {
        (void)dir;
        path.clear();
        const std::filesystem::path cov_path =
            std::filesystem::path(store_dir_) / run_id / "cumulative-cov.bin";
        std::error_code ec;
        if (std::filesystem::exists(cov_path, ec)) {
            path = cov_path.string();
        }
        return true;
    }

    bool saveCumulativeCov(const std::string& run_id, int wave,
                           const std::string& cov_path) override {
        (void)wave;
        const std::filesystem::path dir(store_dir_);
        std::error_code ec;
        std::filesystem::create_directories(dir / run_id, ec);
        if (ec) {
            LOG_ERROR("coverage store: cannot create run dir: " << ec.message());
            return false;
        }
        std::filesystem::copy_file(
            cov_path, dir / run_id / "cumulative-cov.bin",
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            LOG_ERROR("coverage store: copy cumulative cov failed: " << ec.message());
            return false;
        }
        return true;
    }

    bool saveWaveSummary(const std::string& run_id, int wave, std::int64_t covered_lines,
                         std::int64_t total_lines, double percent) override {
        nlohmann::json json;
        json["wave"] = wave;
        json["covered_lines"] = covered_lines;
        json["total_lines"] = total_lines;
        json["percent"] = percent;
        const std::filesystem::path dir(store_dir_);
        std::error_code ec;
        std::filesystem::create_directories(dir / run_id, ec);
        if (ec) {
            LOG_ERROR("coverage store: cannot create run dir: " << ec.message());
            return false;
        }
        std::ofstream out(
            (dir / run_id / ("wave-summary-" + std::to_string(wave) + ".json")).string());
        if (!out.is_open()) {
            LOG_ERROR("coverage store: cannot open wave summary " << wave);
            return false;
        }
        out << json.dump();
        out.flush();
        if (!out.good()) {
            LOG_ERROR("coverage store: write wave summary failed " << wave);
            return false;
        }
        return true;
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

    bool saveFileCoverage(const std::string& run_id, int wave, const std::string& file,
                          const std::vector<std::uint64_t>& points) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return PgExec(conn_,
                      "INSERT INTO coverage_wave_points(run_id, wave_id, file, points) "
                      "VALUES($1,$2,$3,$4) "
                      "ON CONFLICT (run_id, wave_id, file) DO UPDATE SET points=EXCLUDED.points",
                      {{run_id, false}, {std::to_string(wave), false}, {file, false},
                       {PackPoints(points), true}});
    }

    bool saveFileCov(const std::string& run_id, int wave, const std::string& file,
                     const std::string& cov_data, std::int64_t size_bytes) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return PgExec(conn_,
                      "INSERT INTO coverage_cov_files(run_id, wave_id, file, cov, size_bytes) "
                      "VALUES($1,$2,$3,$4,$5) "
                      "ON CONFLICT (run_id, wave_id, file) DO UPDATE "
                      "SET cov=EXCLUDED.cov, size_bytes=EXCLUDED.size_bytes",
                      {{run_id, false}, {std::to_string(wave), false}, {file, false},
                       {cov_data, true}, {std::to_string(size_bytes), false}});
    }

    bool loadWave(const std::string& run_id, int wave,
                  std::unordered_map<std::string, std::set<std::uint64_t>>* file_points)
        override {
        if (file_points == nullptr) {
            return false;
        }
        file_points->clear();
        std::lock_guard<std::mutex> lock(mutex_);
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
                std::set<std::uint64_t>& points = (*file_points)[file];
                for (std::uint64_t p : UnpackPoints(blob)) {
                    points.insert(p);
                }
            }
            PQclear(result);
            return true;
        } else {
            LOG_ERROR("coverage store: loadWave failed: "
                      << (result ? PQresultErrorMessage(result) : PQerrorMessage(conn_)));
            PQclear(result);
            return false;
        }
    }

    bool loadFileCoverage(const std::string& run_id, int wave, const std::string& file,
                          std::set<std::uint64_t>* points) override {
        if (points == nullptr) {
            return false;
        }
        points->clear();
        std::lock_guard<std::mutex> lock(mutex_);
        const char* sql = "SELECT points FROM coverage_wave_points "
                          "WHERE run_id=$1 AND wave_id=$2 AND file=$3";
        const std::vector<PgParam> params = {
            {run_id, false}, {std::to_string(wave), false}, {file, false}};
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
        if (result == nullptr || PQresultStatus(result) != PGRES_TUPLES_OK) {
            LOG_ERROR("coverage store: loadFileCoverage failed: "
                      << (result ? PQresultErrorMessage(result) : PQerrorMessage(conn_)));
            PQclear(result);
            return false;
        }
        const bool found = PQntuples(result) > 0;
        if (found) {
            const std::string blob =
                DecodeByteaText(PQgetvalue(result, 0, 0), PQgetlength(result, 0, 0));
            for (std::uint64_t p : UnpackPoints(blob)) {
                points->insert(p);
            }
        }
        PQclear(result);
        return found;
    }

    bool clearWave(const std::string& run_id, int wave) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool points_ok =
            PgExec(conn_, "DELETE FROM coverage_wave_points WHERE run_id=$1 AND wave_id=$2",
                   {{run_id, false}, {std::to_string(wave), false}});
        const bool cov_ok =
            PgExec(conn_, "DELETE FROM coverage_cov_files WHERE run_id=$1 AND wave_id=$2",
                   {{run_id, false}, {std::to_string(wave), false}});
        return points_ok && cov_ok;
    }

    bool appendSelected(const std::string& run_id,
                        const std::vector<std::string>& files) override {
        std::lock_guard<std::mutex> lock(mutex_);
        bool ok = true;
        for (const std::string& file : files) {
            ok = PgExec(conn_,
                        "INSERT INTO coverage_selected(run_id, file) VALUES($1,$2) "
                        "ON CONFLICT (run_id, file) DO NOTHING",
                        {{run_id, false}, {file, false}}) &&
                 ok;
        }
        return ok;
    }

    bool saveCumulative(const std::string& run_id, int wave,
                        const std::vector<std::uint64_t>& points) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return PgExec(conn_,
                      "INSERT INTO coverage_cumulative(run_id, wave_id, points) "
                      "VALUES($1,$2,$3) "
                      "ON CONFLICT (run_id, wave_id) DO UPDATE SET points=EXCLUDED.points",
                      {{run_id, false}, {std::to_string(wave), false},
                       {PackPoints(points), true}});
    }

    bool saveRunSummary(const std::string& run_id, int total_files, int selected_files,
                        std::int64_t cumulative_points) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return PgExec(conn_,
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

    bool saveCheckpoint(const std::string& run_id, const std::string& blob) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return PgExec(conn_,
                      "INSERT INTO coverage_checkpoints(run_id, blob) VALUES($1,$2) "
                      "ON CONFLICT (run_id) DO UPDATE SET blob=EXCLUDED.blob, "
                      "updated_at=now()",
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

    bool loadCumulative(const std::string& run_id, int wave,
                        std::vector<std::uint64_t>* out) override {
        if (out == nullptr) {
            return false;
        }
        out->clear();
        if (wave < 0) {
            return true;
        }
        std::lock_guard<std::mutex> lock(mutex_);
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
                out->push_back(p);
            }
            PQclear(result);
            return true;
        } else {
            LOG_ERROR("coverage store: loadCumulative failed: "
                      << (result ? PQresultErrorMessage(result) : PQerrorMessage(conn_)));
            PQclear(result);
            return false;
        }
    }

    bool exportWaveCovs(const std::string& run_id, int wave, const std::string& dir,
                        std::vector<std::string>& paths) override {
        paths.clear();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            LOG_ERROR("coverage store: cannot create cov work dir: " << ec.message());
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        const char* sql =
            "SELECT cov FROM coverage_cov_files WHERE run_id=$1 AND wave_id=$2";
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
        if (result == nullptr || PQresultStatus(result) != PGRES_TUPLES_OK) {
            LOG_ERROR("coverage store: exportWaveCovs failed: "
                      << (result ? PQresultErrorMessage(result) : PQerrorMessage(conn_)));
            PQclear(result);
            return false;
        }
        const int rows = PQntuples(result);
        for (int i = 0; i < rows; ++i) {
            const std::string blob =
                DecodeByteaText(PQgetvalue(result, i, 0), PQgetlength(result, i, 0));
            const std::string path =
                (std::filesystem::path(dir) / ("cov-" + std::to_string(i) + ".cov"))
                    .string();
            std::ofstream out(path, std::ios::binary);
            if (!out.is_open() || !out.write(blob.data(),
                                             static_cast<std::streamsize>(blob.size()))) {
                LOG_ERROR("coverage store: write exported cov failed: " << path);
                PQclear(result);
                return false;
            }
            out.flush();
            if (!out.good()) {
                LOG_ERROR("coverage store: flush exported cov failed: " << path);
                PQclear(result);
                return false;
            }
            paths.push_back(path);
        }
        PQclear(result);
        return true;
    }

    bool exportCumulativeCov(const std::string& run_id, const std::string& dir,
                             std::string& path) override {
        path.clear();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            LOG_ERROR("coverage store: cannot create cov work dir: " << ec.message());
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        const char* sql = "SELECT cov FROM coverage_cumulative_cov WHERE run_id=$1";
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
        if (result == nullptr || PQresultStatus(result) != PGRES_TUPLES_OK) {
            LOG_ERROR("coverage store: exportCumulativeCov failed: "
                      << (result ? PQresultErrorMessage(result) : PQerrorMessage(conn_)));
            PQclear(result);
            return false;
        }
        if (PQntuples(result) > 0) {
            const std::string blob =
                DecodeByteaText(PQgetvalue(result, 0, 0), PQgetlength(result, 0, 0));
            const std::string out_path = (std::filesystem::path(dir) / "cumulative.cov")
                                             .string();
            std::ofstream out(out_path, std::ios::binary);
            if (!out.is_open() || !out.write(blob.data(),
                                             static_cast<std::streamsize>(blob.size()))) {
                LOG_ERROR("coverage store: write exported cumulative cov failed");
                PQclear(result);
                return false;
            }
            out.flush();
            if (!out.good()) {
                LOG_ERROR("coverage store: flush exported cumulative cov failed");
                PQclear(result);
                return false;
            }
            path = out_path;
        }
        PQclear(result);
        return true;
    }

    bool saveCumulativeCov(const std::string& run_id, int wave,
                           const std::string& cov_path) override {
        std::ifstream in(cov_path, std::ios::binary | std::ios::ate);
        if (!in.is_open()) {
            LOG_ERROR("coverage store: cannot read merged cov " << cov_path);
            return false;
        }
        const std::streamsize size = in.tellg();
        std::string data(static_cast<size_t>(size), '\0');
        in.seekg(0);
        in.read(&data[0], size);
        if (!in.good()) {
            LOG_ERROR("coverage store: read merged cov failed " << cov_path);
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return PgExec(conn_,
                      "INSERT INTO coverage_cumulative_cov(run_id, wave_id, cov) "
                      "VALUES($1,$2,$3) "
                      "ON CONFLICT (run_id) DO UPDATE SET wave_id=EXCLUDED.wave_id, "
                      "cov=EXCLUDED.cov, updated_at=now()",
                      {{run_id, false}, {std::to_string(wave), false}, {data, true}});
    }

    bool saveWaveSummary(const std::string& run_id, int wave, std::int64_t covered_lines,
                         std::int64_t total_lines, double percent) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return PgExec(conn_,
                      "INSERT INTO coverage_wave_summary(run_id, wave_id, covered_lines, "
                      "total_lines, percent) VALUES($1,$2,$3,$4,$5) "
                      "ON CONFLICT (run_id, wave_id) DO UPDATE SET "
                      "covered_lines=EXCLUDED.covered_lines, "
                      "total_lines=EXCLUDED.total_lines, percent=EXCLUDED.percent, "
                      "settled_at=now()",
                      {{run_id, false}, {std::to_string(wave), false},
                       {std::to_string(covered_lines), false},
                       {std::to_string(total_lines), false},
                       {std::to_string(percent), false}});
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
            " updated_at timestamptz DEFAULT now());"
            "CREATE TABLE IF NOT EXISTS coverage_cumulative_cov ("
            " run_id text PRIMARY KEY, wave_id int, cov bytea,"
            " updated_at timestamptz DEFAULT now());"
            "CREATE TABLE IF NOT EXISTS coverage_wave_summary ("
            " run_id text NOT NULL, wave_id int NOT NULL,"
            " covered_lines bigint, total_lines bigint, percent double precision,"
            " settled_at timestamptz DEFAULT now(),"
            " PRIMARY KEY (run_id, wave_id));";
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
