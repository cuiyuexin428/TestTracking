#include "stpcompare/StpStore.h"
#include "common/AtomicFile.h"
#include "common/PgUtil.h"
#include "common/Util.h"
#include <filesystem>
#include <mutex>
#include <json.hpp>
namespace stpcompare {
namespace {
class FileStpStore : public StpResultStore {
public:
    bool init(const std::string& config) override {
        directory_=nlohmann::json::parse(config).value("store_dir",std::string());
        return !directory_.empty();
    }
    bool publish(const FileResult& result) override {
        const auto path=std::filesystem::path(directory_)/result.run_id/
            (std::to_string(common::Fnv1a64(result.file))+".json");
        common::FilePublicationLock lock(path.string()+".lock");
        if(!lock.acquired()) return false;
        std::string bytes;
        if(common::ReadCompleteFile(path,&bytes)) {
            const auto old=nlohmann::json::parse(bytes,nullptr,false);
            if(old.is_object() && old.value("status",std::string())=="success")
                return old.value("file", std::string()) == result.file;
        }
        const nlohmann::json output={{"file",result.file},{"attempt_id",result.attempt_id},
            {"status",result.successful?"success":"failed"},{"detail",result.detail},
            {"info_txt",result.info_txt},{"info_new_txt",result.info_new_txt}};
        return common::WriteAtomicFile(path,output.dump());
    }
private:
    std::string directory_;
};
#ifdef _WIN32
class PostgresStpStore : public StpResultStore {
public:
    bool init(const std::string& config) override {
        const auto cfg=nlohmann::json::parse(config);
        table_=cfg.value("table",std::string("stp_compare_results"));
        if(!common::IsValidIdentifier(table_) || !connection_.connect(cfg.value("db_conn",std::string()))) return false;
        auto conn=connection_.get();
        return common::EnsurePluginResultSchema(conn) && common::PgExec(conn,
            ("CREATE TABLE IF NOT EXISTS "+table_+"(run_id text NOT NULL,file text NOT NULL,"
             "info_txt text,info_new_txt text,created_at timestamptz DEFAULT now(),PRIMARY KEY(run_id,file))").c_str(),{});
    }
    bool publish(const FileResult& result) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto conn=connection_.get(); common::PgTransaction transaction(conn); bool accepted=false;
        if(!transaction.active() || !common::LockFileOutcome(conn,"stpcompare",result.run_id,result.file,&accepted)) return false;
        if(accepted) return transaction.commit();
        if(result.successful && !common::PgExec(conn,
            ("INSERT INTO "+table_+"(run_id,file,info_txt,info_new_txt) VALUES($1,$2,$3,$4)"
             " ON CONFLICT(run_id,file) DO UPDATE SET info_txt=EXCLUDED.info_txt,info_new_txt=EXCLUDED.info_new_txt,created_at=now()").c_str(),
             {{result.run_id,false},{result.file,false},{result.info_txt,false},{result.info_new_txt,false}})) return false;
        return common::SaveFileOutcome(conn,"stpcompare",result.run_id,result.file,result.attempt_id,
                                       result.successful,result.detail) && transaction.commit();
    }
private:
    common::PgConnection connection_;
    std::mutex mutex_;
    std::string table_;
};
#endif
}
std::unique_ptr<StpResultStore> CreateStpResultStore(const std::string& config) {
#ifdef _WIN32
    if(nlohmann::json::parse(config).value("store_type",std::string("file"))=="postgres") return std::make_unique<PostgresStpStore>();
#endif
    return std::make_unique<FileStpStore>();
}
}
