#include "coverage/CoverageStore.h"
#include "coverage/CoverageUtil.h"
#include "common/AtomicFile.h"
#include "common/PgUtil.h"
#include "common/Util.h"
#include <filesystem>
#include <mutex>
#include <json.hpp>
namespace coverage_plugin {
namespace {
bool FilePoints(const nlohmann::json& record,const std::string& file,
                std::set<std::uint64_t>* points) {
    if (!record.is_object() || !record.contains("file") || !record["file"].is_string() ||
        record["file"]!=file || record.value("status",std::string("success"))!="success" ||
        !record.contains("points") || !record["points"].is_array()) return false;
    std::set<std::uint64_t> loaded;
    for (const auto& point:record["points"]) {
        if (!point.is_number_unsigned()) return false;
        loaded.insert(point.get<std::uint64_t>());
    }
    *points=std::move(loaded); return true;
}
class FileCoverageStore : public CoverageResultStore {
public:
    bool init(const std::string& config) override {
        directory_=nlohmann::json::parse(config).value("store_dir",std::string());
        return !directory_.empty();
    }
    bool publish(FileCoverageResult& result) override {
        const auto path=pathFor(result.run_id,result.file);
        common::FilePublicationLock lock(path.string()+".lock");
        if (!lock.acquired()) return false;
        const auto existing=read(path);
        if (existing.is_object() && existing.value("status",std::string())=="success") {
            std::set<std::uint64_t> points;
            if (!FilePoints(existing,result.file,&points)) return false;
            result.points.assign(points.begin(),points.end());
            result.successful=true;
            result.detail.clear();
            return true;
        }
        nlohmann::json record={{"file",result.file},{"attempt_id",result.attempt_id},
            {"status",result.successful?"success":"failed"},{"detail",result.detail}};
        if (result.successful) {
            record["points"]=result.points;
            record["cov"]=std::vector<unsigned char>(result.cov.begin(),result.cov.end());
        }
        return common::WriteAtomicFile(path,record.dump());
    }
    bool loadFileCoverage(const std::string& run,const std::string& file,std::set<std::uint64_t>* points) override {
        if (!points) return false;
        const auto record=read(pathFor(run,file));
        return FilePoints(record,file,points);
    }
    bool hasFileCov(const std::string& run,const std::string& file) override {
        const auto record=read(pathFor(run,file));
        return record.is_object() && record.contains("cov") && record["cov"].is_array() && !record["cov"].empty();
    }
private:
    std::filesystem::path pathFor(const std::string& run,const std::string& file) const {
        return std::filesystem::path(directory_)/run/(std::to_string(common::Fnv1a64(file))+".json");
    }
    nlohmann::json read(const std::filesystem::path& path) {
        std::string bytes;
        if (!common::ReadCompleteFile(path,&bytes)) return {};
        return nlohmann::json::parse(bytes,nullptr,false);
    }
    std::string directory_;
};
#ifdef _WIN32
class PostgresCoverageStore : public CoverageResultStore {
public:
    bool init(const std::string& config) override {
        const auto cfg=nlohmann::json::parse(config);
        if (!connection_.connect(cfg.value("db_conn",std::string()))) return false;
        auto conn=connection_.get();
        return common::EnsurePluginResultSchema(conn) && common::PgInitializeSchema(conn,
            "CREATE TABLE IF NOT EXISTS coverage_file_points(run_id text NOT NULL,file text NOT NULL,"
            "points bytea NOT NULL,PRIMARY KEY(run_id,file));"
            "CREATE TABLE IF NOT EXISTS coverage_cov_files(run_id text NOT NULL,file text NOT NULL,"
            "cov bytea,size_bytes bigint,PRIMARY KEY(run_id,file));");
    }
    bool publish(FileCoverageResult& result) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto conn=connection_.get(); common::PgTransaction transaction(conn);
        bool accepted=false;
        if (!transaction.active() || !common::LockFileOutcome(conn,"coverage",result.run_id,result.file,&accepted)) return false;
        if (accepted) {
            // Read under the same outcome lock so a duplicate reports exactly
            // the successful publication that caused this attempt to lose.
            auto stored=common::PgQuery(conn,
                "SELECT points FROM coverage_file_points WHERE run_id=$1 AND file=$2",
                {{result.run_id,false},{result.file,false}},true);
            if (!stored || PQresultStatus(stored.get())!=PGRES_TUPLES_OK ||
                PQntuples(stored.get())!=1 || PQgetisnull(stored.get(),0,0)) return false;
            const auto bytes=common::PgCell(stored.get(),0,0);
            if (bytes.size()%sizeof(std::uint64_t)) return false;
            const auto unpacked=UnpackPoints(bytes);
            const std::set<std::uint64_t> points(unpacked.begin(),unpacked.end());
            if (!transaction.commit()) return false;
            result.points.assign(points.begin(),points.end());
            result.successful=true;
            result.detail.clear();
            return true;
        }
        const std::vector<common::PgParam> identity={{result.run_id,false},{result.file,false}};
        if (result.successful) {
            if (!common::PgExec(conn,
                "INSERT INTO coverage_file_points(run_id,file,points) VALUES($1,$2,$3)"
                " ON CONFLICT(run_id,file) DO UPDATE SET points=EXCLUDED.points",
                {{result.run_id,false},{result.file,false},{PackPoints(result.points),true}}) ||
                !common::PgExec(conn,"DELETE FROM coverage_cov_files WHERE run_id=$1 AND file=$2",identity)) return false;
            if (!result.cov.empty() && !common::PgExec(conn,
                "INSERT INTO coverage_cov_files(run_id,file,cov,size_bytes) VALUES($1,$2,$3,$4)",
                {{result.run_id,false},{result.file,false},{result.cov,true},{std::to_string(result.cov.size()),false}})) return false;
        }
        return common::SaveFileOutcome(conn,"coverage",result.run_id,result.file,result.attempt_id,
                                       result.successful,result.detail) && transaction.commit();
    }
    bool loadFileCoverage(const std::string& run,const std::string& file,std::set<std::uint64_t>* points) override {
        if (!points) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        auto result=common::PgQuery(connection_.get(),
            "SELECT points FROM coverage_file_points WHERE run_id=$1 AND file=$2",{{run,false},{file,false}},true);
        if (!result || PQresultStatus(result.get())!=PGRES_TUPLES_OK || PQntuples(result.get())!=1 ||
            PQgetisnull(result.get(),0,0)) return false;
        const auto bytes=common::PgCell(result.get(),0,0);
        if (bytes.size()%sizeof(std::uint64_t)) return false;
        const auto unpacked=UnpackPoints(bytes); points->clear(); points->insert(unpacked.begin(),unpacked.end()); return true;
    }
    bool hasFileCov(const std::string& run,const std::string& file) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto result=common::PgQuery(connection_.get(),
            "SELECT 1 FROM coverage_cov_files WHERE run_id=$1 AND file=$2 AND size_bytes>0 AND octet_length(cov)=size_bytes",
            {{run,false},{file,false}});
        return result && PQresultStatus(result.get())==PGRES_TUPLES_OK && PQntuples(result.get())==1;
    }
private:
    common::PgConnection connection_;
    std::mutex mutex_;
};
#endif
}
std::unique_ptr<CoverageResultStore> CreateCoverageResultStore(const std::string& config) {
#ifdef _WIN32
    if (nlohmann::json::parse(config).value("store_type",std::string("file"))=="postgres")
        return std::make_unique<PostgresCoverageStore>();
#endif
    return std::make_unique<FileCoverageStore>();
}
}
