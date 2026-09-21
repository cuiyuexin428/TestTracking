#include "stpcompare/StpStore.h"
#include "common/AtomicFile.h"
#include "common/PgUtil.h"
#include "common/Util.h"
#include "stpcompare/StpUtil.h"
#include <filesystem>
#include <mutex>
#include <set>
#include <json.hpp>
namespace stpcompare {
namespace {
bool CompleteOutputs(const FileResult& result) {
    if(!result.successful || result.outputs.size()!=4) return false;
    std::set<std::pair<std::string,std::string>> keys;
    for(const auto& output:result.outputs) {
        if((output.program_role!="baseline" && output.program_role!="candidate") ||
           (output.export_stage!="first" && output.export_stage!="second") ||
           output.original_filename!=(output.export_stage=="first"?"info.txt":"info_new.txt") ||
           output.content_size!=output.content.size() ||
           output.content_sha256!=common::Sha256Hex(output.content) ||
           !keys.emplace(output.program_role,output.export_stage).second) return false;
    }
    return keys.size()==4;
}

bool CompleteFileRecord(const nlohmann::json& record,const std::string& file) {
    if(!record.is_object() || record.value("file",std::string())!=file ||
       record.value("status",std::string())!="success" ||
       !record.contains("outputs") || !record["outputs"].is_array() ||
       record["outputs"].size()!=4) return false;
    std::set<std::pair<std::string,std::string>> keys;
    for(const auto& item:record["outputs"]) {
        if(!item.is_object()) return false;
        const auto role=item.value("program_role",std::string());
        const auto stage=item.value("export_stage",std::string());
        const auto hex=item.value("content_hex",std::string());
        std::string content;
        if((role!="baseline"&&role!="candidate") || (stage!="first"&&stage!="second") ||
           !HexDecode(hex,&content) || item.value("content_size",std::uint64_t(-1))!=content.size() ||
           item.value("content_sha256",std::string())!=common::Sha256Hex(content) ||
           !keys.emplace(role,stage).second) return false;
    }
    return keys.size()==4;
}

class FileStpStore : public StpResultStore {
public:
    bool init(const std::string& config) override {
        directory_=nlohmann::json::parse(config).value("store_dir",std::string());
        return !directory_.empty();
    }
    bool hasSuccessfulResult(const std::string& run, const std::string& file) override {
        std::string bytes;
        if (!common::ReadCompleteFile(std::filesystem::path(directory_)/run/
            (std::to_string(common::Fnv1a64(file))+".json"),&bytes)) return false;
        const auto record=nlohmann::json::parse(bytes,nullptr,false);
        return CompleteFileRecord(record,file);
    }
    bool publish(FileResult& result) override {
        const auto path=std::filesystem::path(directory_)/result.run_id/
            (std::to_string(common::Fnv1a64(result.file))+".json");
        common::FilePublicationLock lock(path.string()+".lock");
        if(!lock.acquired()) return false;
        std::string bytes;
        if(common::ReadCompleteFile(path,&bytes)) {
            const auto old=nlohmann::json::parse(bytes,nullptr,false);
            if(CompleteFileRecord(old,result.file)) {
                result.successful=true;
                result.detail.clear();
                return true;
            }
        }
        if(result.successful && !CompleteOutputs(result)) return false;
        nlohmann::json outputs=nlohmann::json::array();
        for(const auto& raw:result.outputs) outputs.push_back({
            {"program_role",raw.program_role},{"export_stage",raw.export_stage},
            {"program_version",raw.program_version},{"original_filename",raw.original_filename},
            {"content_hex",HexEncode(raw.content)},{"content_sha256",raw.content_sha256},
            {"content_size",raw.content_size},{"attempt_id",result.attempt_id}});
        const nlohmann::json output={{"file",result.file},{"attempt_id",result.attempt_id},
            {"status",result.successful?"success":"failed"},{"detail",result.detail},
            {"outputs",outputs}};
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
        const auto index=table_+"_raw_output_key";
        const auto ddl="CREATE TABLE IF NOT EXISTS "+table_+"(run_id text NOT NULL,file text NOT NULL,"
            "program_role text NOT NULL,export_stage text NOT NULL,program_version text NOT NULL DEFAULT '',"
            "original_filename text NOT NULL,content bytea NOT NULL,content_sha256 text NOT NULL,"
            "content_size bigint NOT NULL,attempt_id text NOT NULL DEFAULT '',"
            "created_at timestamptz DEFAULT now());"
            "ALTER TABLE "+table_+" ADD COLUMN IF NOT EXISTS program_role text;"
            "ALTER TABLE "+table_+" ADD COLUMN IF NOT EXISTS export_stage text;"
            "ALTER TABLE "+table_+" ADD COLUMN IF NOT EXISTS program_version text;"
            "ALTER TABLE "+table_+" ADD COLUMN IF NOT EXISTS original_filename text;"
            "ALTER TABLE "+table_+" ADD COLUMN IF NOT EXISTS content bytea;"
            "ALTER TABLE "+table_+" ADD COLUMN IF NOT EXISTS content_sha256 text;"
            "ALTER TABLE "+table_+" ADD COLUMN IF NOT EXISTS content_size bigint;"
            "ALTER TABLE "+table_+" ADD COLUMN IF NOT EXISTS attempt_id text;"
            "ALTER TABLE "+table_+" ADD COLUMN IF NOT EXISTS created_at timestamptz DEFAULT now();"
            "DO $$ DECLARE old_pk text; BEGIN SELECT conname INTO old_pk FROM pg_constraint "
            "WHERE conrelid='"+table_+"'::regclass AND contype='p' "
            "AND pg_get_constraintdef(oid)='PRIMARY KEY (run_id, file)'; "
            "IF old_pk IS NOT NULL THEN EXECUTE 'ALTER TABLE "+table_+
            " DROP CONSTRAINT '||quote_ident(old_pk); END IF; END $$;"
            "CREATE UNIQUE INDEX IF NOT EXISTS "+index+" ON "+table_+
            "(run_id,file,program_role,export_stage) WHERE program_role IS NOT NULL AND export_stage IS NOT NULL;";
        return common::EnsurePluginResultSchema(conn) && common::PgInitializeSchema(conn,ddl.c_str());
    }
    bool hasSuccessfulResult(const std::string& run, const std::string& file) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto result=common::PgQuery(connection_.get(),
            ("SELECT 1 FROM plugin_file_outcomes o JOIN "+table_+
             " r ON r.run_id=o.run_id AND r.file=o.file "
             "WHERE o.plugin_type='stpcompare' AND o.run_id=$1 AND o.file=$2 "
             "AND o.status='success' AND r.content IS NOT NULL AND r.content_sha256 IS NOT NULL "
             "GROUP BY o.run_id,o.file HAVING count(*)=4 AND "
             "count(DISTINCT r.program_role||':'||r.export_stage)=4").c_str(),
            {{run,false},{file,false}});
        return result && PQresultStatus(result.get())==PGRES_TUPLES_OK && PQntuples(result.get())==1;
    }
    bool publish(FileResult& result) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto conn=connection_.get(); common::PgTransaction transaction(conn); bool accepted=false;
        if(!transaction.active() || !common::LockFileOutcome(conn,"stpcompare",result.run_id,result.file,&accepted)) return false;
        if(accepted) {
            auto complete=common::PgQuery(conn,("SELECT count(*)::text FROM "+table_+
                " WHERE run_id=$1 AND file=$2 AND content IS NOT NULL AND content_sha256 IS NOT NULL "
                "AND program_role IN ('baseline','candidate') AND export_stage IN ('first','second')").c_str(),
                {{result.run_id,false},{result.file,false}});
            if(!complete || PQresultStatus(complete.get())!=PGRES_TUPLES_OK ||
               common::PgCell(complete.get(),0,0)!="4" || !transaction.commit()) return false;
            result.successful=true;
            result.detail.clear();
            return true;
        }
        if(result.successful) {
            if(!CompleteOutputs(result)) return false;
            for(const auto& raw:result.outputs) {
                if(!common::PgExec(conn,("INSERT INTO "+table_+
                    "(run_id,file,program_role,export_stage,program_version,original_filename,content,"
                    "content_sha256,content_size,attempt_id,created_at) VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,now()) "
                    "ON CONFLICT(run_id,file,program_role,export_stage) WHERE program_role IS NOT NULL "
                    "AND export_stage IS NOT NULL DO UPDATE SET program_version=EXCLUDED.program_version,"
                    "original_filename=EXCLUDED.original_filename,content=EXCLUDED.content,"
                    "content_sha256=EXCLUDED.content_sha256,content_size=EXCLUDED.content_size,"
                    "attempt_id=EXCLUDED.attempt_id,created_at=now()").c_str(),
                    {{result.run_id,false},{result.file,false},{raw.program_role,false},
                     {raw.export_stage,false},{raw.program_version,false},{raw.original_filename,false},
                     {raw.content,true},{raw.content_sha256,false},{std::to_string(raw.content_size),false},
                     {result.attempt_id,false}})) return false;
            }
        }
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
