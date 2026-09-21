#include "common/RunStore.h"
#include "common/AtomicFile.h"
#include "common/TaskPlugin.h"
#include "common/Util.h"
#include <filesystem>
#include <json.hpp>

namespace common {
namespace {
std::filesystem::file_status RecoveryFileStatus(const std::filesystem::path& path) {
    std::error_code error;
    auto status = std::filesystem::status(path,error);
    if (error && error != std::errc::no_such_file_or_directory)
        throw RecoveryBlocked("cannot inspect recovery file " + path.u8string() + ": " + error.message());
    return status;
}
bool ReadRecoveryFile(const std::filesystem::path& path, std::string* bytes,
                       std::size_t maximum_size = 512ULL * 1024 * 1024) {
    const auto status = RecoveryFileStatus(path);
    if (!std::filesystem::exists(status) || !std::filesystem::is_regular_file(status)) return false;
    std::error_code error;
    const auto size = std::filesystem::file_size(path,error);
    if (error) throw RecoveryBlocked("cannot inspect recovery file " + path.u8string() + ": " + error.message());
    if (size > maximum_size) return false;
    if (!ReadCompleteFile(path,bytes,maximum_size))
        throw RecoveryBlocked("cannot read recovery file " + path.u8string());
    return true;
}
}
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
                        const std::vector<ManifestEntry>& files, const std::string& fingerprint,
                        std::size_t planned_tasks, const std::string& dataset_version) {
    const auto& effective_dataset = dataset_version.empty() ? dataset_version_ : dataset_version;
#ifdef _WIN32
    if (postgres_) {
        auto conn = connection_.get();
        PgTransaction transaction(conn);
        if (!transaction.active() || !PgExec(conn,
            ("INSERT INTO " + checkpoint_table_ + "(run_id,blob) VALUES($1,$2) "
             "ON CONFLICT(run_id) DO UPDATE SET blob=EXCLUDED.blob,updated_at=now()").c_str(),
            {{run,false},{blob,true}}) || !PgExec(conn,
            "INSERT INTO plugin_runs(run_id,plugin_type,expected_files,planned_tasks,manifest_fingerprint,program_version,dataset_version,status) "
            "VALUES($1,$2,$3,$4,$5,$6,$7,'planned') ON CONFLICT(run_id,plugin_type) DO UPDATE SET "
            "expected_files=EXCLUDED.expected_files,planned_tasks=EXCLUDED.planned_tasks,"
            "manifest_fingerprint=EXCLUDED.manifest_fingerprint",
            {{run,false},{plugin_,false},{std::to_string(files.size()),false},
             {std::to_string(planned_tasks),false},{fingerprint,false},
             {program_version_,false},{effective_dataset,false}})) return false;
        // Batched INSERT avoids one round trip per manifest entry.
        nlohmann::json manifest = nlohmann::json::array();
        for (const auto& file : files) {
            manifest.push_back({{"file",file.path},
                {"size_bytes",file.has_size ? nlohmann::json(file.size) : nlohmann::json(nullptr)},
                {"content_sha256",file.content_sha256.empty()
                    ? nlohmann::json(nullptr) : nlohmann::json(file.content_sha256)}});
        }
        if (!PgExec(conn,
            "INSERT INTO plugin_run_files(run_id,plugin_type,file,size_bytes,content_sha256) "
            "SELECT $1,$2,item.file,item.size_bytes,item.content_sha256 FROM "
            "jsonb_to_recordset($3::jsonb) AS item(file text,size_bytes bigint,content_sha256 text) "
            "ON CONFLICT DO NOTHING",
            {{run,false},{plugin_,false},{manifest.dump(),false}})) return false;
        return transaction.commit();
    }
#endif
    const auto directory = std::filesystem::path(directory_) / run;
    nlohmann::json paths=nlohmann::json::array(), file_metadata=nlohmann::json::array();
    for (const auto& file : files) {
        paths.push_back(file.path);
        file_metadata.push_back({{"file",file.path},
            {"size_bytes",file.has_size ? nlohmann::json(file.size) : nlohmann::json(nullptr)},
            {"content_sha256",file.content_sha256.empty()
                ? nlohmann::json(nullptr) : nlohmann::json(file.content_sha256)}});
    }
    nlohmann::json metadata = {{"run_id",run},{"plugin_type",plugin_},{"expected_files",files.size()},
        {"planned_tasks",planned_tasks},
        {"manifest_fingerprint",fingerprint},{"files",paths},{"file_metadata",file_metadata},{"status","planned"},
        {"program_version",program_version_},{"dataset_version",effective_dataset}};
    return WriteAtomicFile(directory / "checkpoint.bin", blob) &&
           WriteAtomicFile(directory / "run-metadata.json", metadata.dump());
}

bool RunStore::loadPlan(const std::string& run, std::string* blob) {
#ifdef _WIN32
    if (postgres_) {
        auto result = PgQuery(connection_.get(),
            ("SELECT blob FROM " + checkpoint_table_ + " WHERE run_id=$1").c_str(), {{run,false}}, true);
        if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK)
            throw RecoveryBlocked("cannot read checkpoint for run " + run + " from " + checkpoint_table_);
        if (PQntuples(result.get()) != 1) return false;
        *blob = PgCell(result.get(),0,0);
        return true;
    }
#endif
    return ReadRecoveryFile(std::filesystem::path(directory_) / run / "checkpoint.bin", blob);
}

bool RunStore::loadIdentity(const std::string& run, RunIdentity* identity) {
    if (!identity) return false;
#ifdef _WIN32
    if (postgres_) {
        auto result = PgQuery(connection_.get(),
            "SELECT program_version,dataset_version,manifest_fingerprint FROM plugin_runs "
            "WHERE run_id=$1 AND plugin_type=$2", {{run,false},{plugin_,false}});
        if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK)
            throw RecoveryBlocked("cannot read saved deployment identity for run " + run);
        if (PQntuples(result.get()) != 1) return false;
        *identity = {PgCell(result.get(),0,0), PgCell(result.get(),0,1), PgCell(result.get(),0,2)};
        return true;
    }
#endif
    std::string bytes;
    if (!ReadRecoveryFile(std::filesystem::path(directory_) / run / "run-metadata.json", &bytes))
        return false;
    const auto metadata = nlohmann::json::parse(bytes,nullptr,false);
    if (!metadata.is_object() || metadata.value("run_id",std::string()) != run ||
        metadata.value("plugin_type",std::string()) != plugin_) return false;
    *identity = {metadata.value("program_version",std::string()),
                 metadata.value("dataset_version",std::string()),
                 metadata.value("manifest_fingerprint",std::string())};
    return true;
}

bool RunStore::recordCompletion(const std::string& run, const std::string& task,
                                 std::optional<int> exit_code) {
#ifdef _WIN32
    if (postgres_) return PgExec(connection_.get(),
        "INSERT INTO plugin_task_completions(run_id,plugin_type,task_id,exit_code) "
        "VALUES($1,$2,$3,NULLIF($4,'')::integer) ON CONFLICT DO NOTHING",
        {{run,false},{plugin_,false},{task,false},
         {exit_code ? std::to_string(*exit_code) : std::string(),false}});
#endif
    const nlohmann::json completion = {{"version",1},{"task_id",task},
        {"exit_code",exit_code ? nlohmann::json(*exit_code) : nlohmann::json(nullptr)}};
    return WriteAtomicFile(std::filesystem::path(directory_) / run / "completed" /
                            (std::to_string(Fnv1a64(task)) + ".done"), completion.dump());
}

bool RunStore::loadCompletions(const std::string& run, std::set<std::string>* tasks) {
    tasks->clear();
#ifdef _WIN32
    if (postgres_) {
        auto result = PgQuery(connection_.get(),
            "SELECT task_id FROM plugin_task_completions WHERE run_id=$1 AND plugin_type=$2", {{run,false},{plugin_,false}});
        if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK)
            throw RecoveryBlocked("cannot read completion journal for run " + run);
        for (int i=0;i<PQntuples(result.get());++i) tasks->insert(PgCell(result.get(),i,0));
        return true;
    }
#endif
    const auto directory = std::filesystem::path(directory_) / run / "completed";
    const auto status = RecoveryFileStatus(directory);
    if (!std::filesystem::exists(status)) return true;
    if (!std::filesystem::is_directory(status)) return false;
    std::error_code error;
    auto entry = std::filesystem::directory_iterator(directory,error);
    if (error) throw RecoveryBlocked("cannot enumerate completion journal for run " + run + ": " + error.message());
    const auto end = std::filesystem::directory_iterator();
    for (; entry != end; entry.increment(error)) {
        if (error) break;
        if (entry->path().extension() != ".done") continue;
        std::string task;
        if (!ReadRecoveryFile(entry->path(), &task, 1024*1024) || task.empty()) return false;
        // Older journals contain only the task ID. New records also preserve
        // the generic outcome needed to reconcile a committed task after a crash.
        const auto completion = nlohmann::json::parse(task,nullptr,false);
        if (completion.is_object()) {
            if (completion.value("version",0) != 1 || !completion.contains("task_id") ||
                !completion["task_id"].is_string() || completion["task_id"].get_ref<const std::string&>().empty())
                return false;
            task = completion["task_id"].get<std::string>();
        }
        tasks->insert(task);
    }
    if (error) throw RecoveryBlocked("cannot enumerate completion journal for run " + run + ": " + error.message());
    return true;
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
