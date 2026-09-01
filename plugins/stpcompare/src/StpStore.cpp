#include "stpcompare/StpStore.h"

#include "common/Logger.h"
#include "common/PgUtil.h"
#include "common/Util.h"

#include <json.hpp>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <utility>

#ifdef _WIN32
#include <libpq-fe.h>
#endif

namespace stpcompare {

namespace {

std::string SafeFileName(const std::string& file) {
    return std::to_string(common::Fnv1a64(file));
}

class FileStpStore : public StpResultStore, public StpCheckpointStore {
public:
    bool init(const std::string& config) override {
        const nlohmann::json cfg = nlohmann::json::parse(config);
        store_dir_ = cfg.value("store_dir", std::string());
        if (store_dir_.empty()) {
            LOG_ERROR("stp_compare store: store_dir is required for file backend");
            return false;
        }
        std::error_code ec;
        std::filesystem::create_directories(store_dir_, ec);
        if (ec) {
            LOG_ERROR("stp_compare store: cannot create store_dir " << store_dir_ << ": "
                                                                    << ec.message());
            return false;
        }
        return true;
    }

    bool saveFileResult(const std::string& run_id, const std::string& file,
                        const std::string& info_txt,
                        const std::string& info_new_txt) override {
        const std::filesystem::path dir =
            std::filesystem::path(store_dir_) / run_id;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            LOG_ERROR("stp_compare store: cannot create run dir " << dir << ": "
                                                                  << ec.message());
            return false;
        }
        const std::string base = SafeFileName(file);
        std::ofstream info(dir / (base + ".info.txt"), std::ios::binary);
        if (!info.is_open()) {
            LOG_ERROR("stp_compare store: cannot open info.txt for " << file);
            return false;
        }
        info << info_txt;
        info.flush();
        if (!info.good()) {
            LOG_ERROR("stp_compare store: write info.txt failed for " << file);
            return false;
        }
        std::ofstream info_new(dir / (base + ".info_new.txt"), std::ios::binary);
        if (!info_new.is_open()) {
            LOG_ERROR("stp_compare store: cannot open info_new.txt for " << file);
            return false;
        }
        info_new << info_new_txt;
        info_new.flush();
        if (!info_new.good()) {
            LOG_ERROR("stp_compare store: write info_new.txt failed for " << file);
            return false;
        }
        return true;
    }

    bool saveCheckpoint(const std::string& run_id, const std::string& blob) override {
        const std::filesystem::path dir = std::filesystem::path(store_dir_) / run_id;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            LOG_ERROR("stp_compare store: cannot create run dir " << dir << ": "
                                                                  << ec.message());
            return false;
        }
        std::ofstream out((dir / "checkpoint.bin").string(), std::ios::binary);
        if (!out.is_open()) {
            LOG_ERROR("stp_compare store: cannot open checkpoint.bin");
            return false;
        }
        out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
        out.flush();
        if (!out.good()) {
            LOG_ERROR("stp_compare store: write checkpoint.bin failed");
            return false;
        }
        return true;
    }

    bool loadCheckpoint(const std::string& run_id, std::string* blob) override {
        if (blob == nullptr) {
            return false;
        }
        std::ifstream in(
            (std::filesystem::path(store_dir_) / run_id / "checkpoint.bin").string(),
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

class PostgresStpStore : public StpResultStore, public StpCheckpointStore {
public:
    ~PostgresStpStore() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (conn_ != nullptr) {
            PQfinish(conn_);
            conn_ = nullptr;
        }
    }

    bool init(const std::string& config) override {
        const nlohmann::json cfg = nlohmann::json::parse(config);
        const std::string conninfo = cfg.value("db_conn", std::string());
        const std::string table = cfg.value("table", std::string());
        if (conninfo.empty()) {
            LOG_ERROR("stp_compare store: postgres requires db_conn");
            return false;
        }
        if (!table.empty() && !common::IsValidIdentifier(table)) {
            LOG_ERROR("stp_compare store: invalid table name: " << table);
            return false;
        }
        table_ = table;
        std::lock_guard<std::mutex> lock(mutex_);
        conn_ = PQconnectdb(conninfo.c_str());
        if (conn_ == nullptr || PQstatus(conn_) != CONNECTION_OK) {
            LOG_ERROR("stp_compare store: postgres connect failed: "
                      << (conn_ ? PQerrorMessage(conn_) : "PQconnectdb returned null"));
            if (conn_ != nullptr) {
                PQfinish(conn_);
                conn_ = nullptr;
            }
            return false;
        }
        return EnsureSchema();
    }

    bool saveFileResult(const std::string& run_id, const std::string& file,
                        const std::string& info_txt,
                        const std::string& info_new_txt) override {
        if (table_.empty()) {
            LOG_ERROR("stp_compare store: no data table configured for client write");
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string sql =
            "INSERT INTO " + table_ +
            "(run_id, file, info_txt, info_new_txt) VALUES($1,$2,$3,$4) "
            "ON CONFLICT (run_id, file) DO UPDATE SET "
            "info_txt=EXCLUDED.info_txt, info_new_txt=EXCLUDED.info_new_txt, "
            "created_at=now()";
        return common::PgExec(conn_, sql.c_str(),
                              {{run_id, false}, {file, false}, {info_txt, false},
                               {info_new_txt, false}});
    }

    bool saveCheckpoint(const std::string& run_id, const std::string& blob) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return common::PgExec(
            conn_,
            "INSERT INTO stp_compare_checkpoints(run_id, blob) VALUES($1,$2) "
            "ON CONFLICT (run_id) DO UPDATE SET blob=EXCLUDED.blob, updated_at=now()",
            {{run_id, false}, {blob, true}});
    }

    bool loadCheckpoint(const std::string& run_id, std::string* blob) override {
        if (blob == nullptr) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        const char* sql = "SELECT blob FROM stp_compare_checkpoints WHERE run_id=$1";
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
            LOG_ERROR("stp_compare store: loadCheckpoint failed: "
                      << (result ? PQresultErrorMessage(result) : PQerrorMessage(conn_)));
        }
        PQclear(result);
        return ok;
    }

private:
    bool EnsureSchema() {
        std::string ddl =
            "CREATE TABLE IF NOT EXISTS stp_compare_checkpoints ("
            " run_id text PRIMARY KEY, blob bytea NOT NULL,"
            " updated_at timestamptz DEFAULT now());";
        std::string data_ddl;
        if (!table_.empty()) {
            data_ddl =
                "CREATE TABLE IF NOT EXISTS " + table_ + " ("
                " run_id text NOT NULL,"
                " file text NOT NULL,"
                " info_txt text,"
                " info_new_txt text,"
                " created_at timestamptz DEFAULT now(),"
                " PRIMARY KEY (run_id, file));";
        }
        PGresult* result = PQexec(conn_, (ddl + data_ddl).c_str());
        bool ok = false;
        if (result != nullptr && PQresultStatus(result) == PGRES_COMMAND_OK) {
            ok = true;
        } else {
            LOG_ERROR("stp_compare store: schema init failed: "
                      << (result ? PQresultErrorMessage(result) : PQerrorMessage(conn_)));
        }
        PQclear(result);
        return ok;
    }

    PGconn* conn_ = nullptr;
    mutable std::mutex mutex_;
    std::string table_;
};

#endif  // _WIN32

}  // namespace

std::unique_ptr<StpResultStore> CreateStpResultStore(const std::string& config) {
    const nlohmann::json cfg = nlohmann::json::parse(config);
#ifdef _WIN32
    if (cfg.value("store_type", std::string("file")) == "postgres") {
        return std::make_unique<PostgresStpStore>();
    }
#else
    (void)cfg;
#endif
    return std::make_unique<FileStpStore>();
}

std::unique_ptr<StpCheckpointStore> CreateStpCheckpointStore(const std::string& config) {
    const nlohmann::json cfg = nlohmann::json::parse(config);
#ifdef _WIN32
    if (cfg.value("store_type", std::string("file")) == "postgres") {
        return std::make_unique<PostgresStpStore>();
    }
#else
    (void)cfg;
#endif
    return std::make_unique<FileStpStore>();
}

}  // namespace stpcompare
