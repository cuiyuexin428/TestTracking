#include "common/RunStore.h"
#include "common/AtomicFile.h"
#include "common/Util.h"
#include <filesystem>
#include <json.hpp>

namespace common {
bool RunStore::init(const std::string& config, const std::string& plugin,
                    const std::string& checkpoint_table) {
    const auto cfg = nlohmann::json::parse(config);
    plugin_ = plugin;
    directory_ = cfg.value("store_dir", std::string());
    program_version_ = cfg.value("expected_program_version", std::string());
    dataset_version_ = cfg.value("expected_dataset_version", std::string());
    if (!IsValidIdentifier(checkpoint_table)) return false;
    checkpoint_table_ = checkpoint_table;
    postgres_ = cfg.value("store_type", std::string("file")) == "postgres";
#ifdef _WIN32
    if (postgres_) {
        if (!connection_.connect(cfg.value("db_conn", std::string()))) return false;
        auto conn = connection_.get();
        PgTransaction transaction(conn);
        if (!transaction.active() || !EnsurePluginResultSchema(conn) || !PgExec(conn,
            ("CREATE TABLE IF NOT EXISTS " + checkpoint_table_ +
             "(run_id text PRIMARY KEY,blob bytea NOT NULL,updated_at timestamptz DEFAULT now())").c_str(), {}) ||
            !PgExec(conn,
                "CREATE TABLE IF NOT EXISTS plugin_checkpoint_tables("
                "plugin_type text NOT NULL,checkpoint_table text NOT NULL,"
                "registered_at timestamptz DEFAULT now(),"
                "PRIMARY KEY(plugin_type,checkpoint_table))", {}) ||
            !PgExec(conn,
                // Record the actual relation name: unquoted SQL folds case.
                "INSERT INTO plugin_checkpoint_tables(plugin_type,checkpoint_table) "
                "SELECT $1,relname FROM pg_class WHERE oid=to_regclass($2) ON CONFLICT DO NOTHING",
                {{plugin_,false},{checkpoint_table_,false}})) return false;
        return transaction.commit();
    }
#endif
    if (postgres_ || directory_.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    return !error;
}

bool RunStore::savePlan(const std::string& run, const std::string& blob,
                        const std::vector<std::string>& files, const std::string& fingerprint) {
#ifdef _WIN32
    if (postgres_) {
        auto conn = connection_.get();
        PgTransaction transaction(conn);
        if (!transaction.active() || !PgExec(conn,
            ("INSERT INTO " + checkpoint_table_ + "(run_id,blob) VALUES($1,$2) "
             "ON CONFLICT(run_id) DO UPDATE SET blob=EXCLUDED.blob,updated_at=now()").c_str(),
            {{run,false},{blob,true}}) || !PgExec(conn,
            "INSERT INTO plugin_runs(run_id,plugin_type,expected_files,manifest_fingerprint,program_version,dataset_version,status) "
            "VALUES($1,$2,$3,$4,$5,$6,'planned') ON CONFLICT(run_id,plugin_type) DO UPDATE SET "
            "expected_files=EXCLUDED.expected_files,manifest_fingerprint=EXCLUDED.manifest_fingerprint",
            {{run,false},{plugin_,false},{std::to_string(files.size()),false},{fingerprint,false},
             {program_version_,false},{dataset_version_,false}})) return false;
        // Batched INSERT avoids one round trip per manifest entry.
        nlohmann::json manifest = files;
        if (!PgExec(conn,
            "INSERT INTO plugin_run_files(run_id,plugin_type,file) SELECT $1,$2,value "
            "FROM jsonb_array_elements_text($3::jsonb) ON CONFLICT DO NOTHING",
            {{run,false},{plugin_,false},{manifest.dump(),false}})) return false;
        return transaction.commit();
    }
#endif
    const auto directory = std::filesystem::path(directory_) / run;
    nlohmann::json metadata = {{"run_id",run},{"plugin_type",plugin_},{"expected_files",files.size()},
        {"manifest_fingerprint",fingerprint},{"files",files},{"status","planned"},
        {"program_version",program_version_},{"dataset_version",dataset_version_}};
    return WriteAtomicFile(directory / "checkpoint.bin", blob) &&
           WriteAtomicFile(directory / "run-metadata.json", metadata.dump());
}

bool RunStore::loadPlan(const std::string& run, std::string* blob) {
#ifdef _WIN32
    if (postgres_) {
        auto result = PgQuery(connection_.get(),
            ("SELECT blob FROM " + checkpoint_table_ + " WHERE run_id=$1").c_str(), {{run,false}}, true);
        if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK || PQntuples(result.get()) != 1) return false;
        *blob = PgCell(result.get(),0,0);
        return true;
    }
#endif
    return ReadCompleteFile(std::filesystem::path(directory_) / run / "checkpoint.bin", blob);
}

bool RunStore::recordCompletion(const std::string& run, const std::string& task) {
#ifdef _WIN32
    if (postgres_) return PgExec(connection_.get(),
        "INSERT INTO plugin_task_completions(run_id,plugin_type,task_id) VALUES($1,$2,$3) ON CONFLICT DO NOTHING",
        {{run,false},{plugin_,false},{task,false}});
#endif
    return WriteAtomicFile(std::filesystem::path(directory_) / run / "completed" /
                            (std::to_string(Fnv1a64(task)) + ".done"), task);
}

bool RunStore::loadCompletions(const std::string& run, std::set<std::string>* tasks) {
    tasks->clear();
#ifdef _WIN32
    if (postgres_) {
        auto result = PgQuery(connection_.get(),
            "SELECT task_id FROM plugin_task_completions WHERE run_id=$1 AND plugin_type=$2", {{run,false},{plugin_,false}});
        if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK) return false;
        for (int i=0;i<PQntuples(result.get());++i) tasks->insert(PgCell(result.get(),i,0));
        return true;
    }
#endif
    const auto directory = std::filesystem::path(directory_) / run / "completed";
    std::error_code error;
    if (!std::filesystem::exists(directory,error)) return !error;
    for (const auto& entry : std::filesystem::directory_iterator(directory,error)) {
        if (entry.path().extension() != ".done") continue;
        std::string task;
        if (!ReadCompleteFile(entry.path(), &task, 1024*1024) || task.empty()) return false;
        tasks->insert(task);
    }
    return !error;
}

bool RunStore::finish(const std::string& run) {
#ifdef _WIN32
    if (postgres_) {
        auto result=PgQuery(connection_.get(),
            "UPDATE plugin_runs SET status='finished',finished_at=COALESCE(finished_at,now()) "
            "WHERE run_id=$1 AND plugin_type=$2 RETURNING run_id",{{run,false},{plugin_,false}});
        return result && PQresultStatus(result.get())==PGRES_TUPLES_OK && PQntuples(result.get())==1;
    }
#endif
    const auto path = std::filesystem::path(directory_) / run / "run-metadata.json";
    std::string bytes;
    if (!ReadCompleteFile(path,&bytes)) return false;
    auto metadata = nlohmann::json::parse(bytes,nullptr,false);
    if (!metadata.is_object() || metadata.value("run_id",std::string())!=run ||
        metadata.value("plugin_type",std::string())!=plugin_ || !metadata.contains("files")) return false;
    if(metadata.value("status",std::string())=="finished") return true;
    metadata["status"] = "finished";
    return WriteAtomicFile(path,metadata.dump());
}
}
