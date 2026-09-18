#include "scheduler/Storage.h"

#include <utility>

#include "common/Logger.h"

namespace scheduler {
bool Storage::init(const std::string& conninfo) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connection_.connect(conninfo)) return false;
    const char* ddl =
        "CREATE TABLE IF NOT EXISTS runs ("
        " run_id text PRIMARY KEY, plugin_type text, dataset_version text, params text,"
        " status text NOT NULL DEFAULT 'running', created_at timestamptz DEFAULT now(), "
        "finished_at timestamptz);"
        "CREATE TABLE IF NOT EXISTS tasks ("
        " task_id text PRIMARY KEY, run_id text, client_identity text, status text,"
        " sent_at timestamptz DEFAULT now(), acked_at timestamptz, reported_at timestamptz,"
        " exit_code int, duration_ms bigint, artifact_path text, result bytea, payload bytea);"
        "DO $$ BEGIN IF NOT EXISTS (SELECT 1 FROM pg_attribute "
        "WHERE attrelid='tasks'::regclass AND attname='attempt_id' AND NOT attisdropped) THEN "
        "ALTER TABLE tasks ADD COLUMN attempt_id text NOT NULL DEFAULT ''; END IF; END $$;"
        "CREATE TABLE IF NOT EXISTS plugin_task_completions("
        " run_id text NOT NULL, plugin_type text NOT NULL, task_id text NOT NULL,"
        " completed_at timestamptz DEFAULT now(), PRIMARY KEY(run_id,plugin_type,task_id));"
        "CREATE INDEX IF NOT EXISTS idx_tasks_run ON tasks(run_id);";
    enabled_ = common::PgInitializeSchema(connection_.get(), ddl);
    return enabled_;
}
bool Storage::Exec(const char* sql, const std::vector<common::PgParam>& params) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = common::PgQuery(connection_.get(), sql, params);
    if (!result || PQresultStatus(result.get()) != PGRES_COMMAND_OK ||
        std::string(PQcmdTuples(result.get())) == "0") {
        LOG_ERROR("storage: write failed: " << (result ? PQresultErrorMessage(result.get())
                                                       : connection_.lastError()));
        return false;
    }
    return true;
}
bool Storage::SaveRun(const std::string& id, const std::string& plugin, const std::string& dataset,
                      const std::string& params) {
    return Exec("INSERT INTO runs(run_id,plugin_type,dataset_version,params,status) "
                "VALUES($1,$2,$3,$4,'planning') ON CONFLICT(run_id) DO UPDATE SET "
                "plugin_type=EXCLUDED.plugin_type,dataset_version=EXCLUDED.dataset_version,"
                "params=EXCLUDED.params,status='planning',finished_at=NULL",
                {{id, false}, {plugin, false}, {dataset, false}, {params, false}});
}
bool Storage::ActivateRun(const std::string& id, const std::string& plugin) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* connection = connection_.get();
    common::PgTransaction transaction(connection);
    if (!transaction.active()) return false;

    // Activation is infrequent. Serializing it at the runs table keeps the
    // single-active-run check valid even if two schedulers race for a plugin.
    auto activation_lock = common::PgResult(
        PQexec(connection, "LOCK TABLE runs IN SHARE ROW EXCLUSIVE MODE"), PQclear);
    if (!activation_lock || PQresultStatus(activation_lock.get()) != PGRES_COMMAND_OK) {
        LOG_ERROR("storage: failed to lock runs for activation: "
                  << (activation_lock ? PQresultErrorMessage(activation_lock.get())
                                      : connection_.lastError()));
        return false;
    }

    auto running = common::PgQuery(
        connection,
        "SELECT 1 FROM runs WHERE plugin_type=$1 AND status='running' AND run_id<>$2 LIMIT 1",
        {{plugin, false}, {id, false}});
    if (!running || PQresultStatus(running.get()) != PGRES_TUPLES_OK) {
        LOG_ERROR("storage: failed to check active run: "
                  << (running ? PQresultErrorMessage(running.get()) : connection_.lastError()));
        return false;
    }
    if (PQntuples(running.get()) != 0) {
        LOG_ERROR("storage: refusing to activate run " << id
                                                        << "; plugin already has a running run");
        return false;
    }

    auto activated = common::PgQuery(
        connection,
        "UPDATE runs SET status='running',finished_at=NULL "
        "WHERE run_id=$1 AND plugin_type=$2 AND status='planning'",
        {{id, false}, {plugin, false}});
    if (!activated || PQresultStatus(activated.get()) != PGRES_COMMAND_OK ||
        std::string(PQcmdTuples(activated.get())) == "0") {
        LOG_ERROR("storage: failed to activate run: "
                  << (activated ? PQresultErrorMessage(activated.get()) : connection_.lastError()));
        return false;
    }

    auto superseded = common::PgQuery(connection,
                                      "UPDATE runs SET status='cancelled',finished_at=now() "
                                      "WHERE plugin_type=$1 AND status='stopped' AND run_id<>$2",
                                      {{plugin, false}, {id, false}});
    if (!superseded || PQresultStatus(superseded.get()) != PGRES_COMMAND_OK) {
        LOG_ERROR(
            "storage: failed to supersede stopped run: "
            << (superseded ? PQresultErrorMessage(superseded.get()) : connection_.lastError()));
        return false;
    }
    return transaction.commit();
}
bool Storage::FinishRun(const std::string& id, const std::string& status) {
    return Exec("UPDATE runs SET status=$2,finished_at=now() WHERE run_id=$1",
                {{id, false}, {status, false}});
}
bool Storage::LoadUnfinishedRuns(std::vector<RunRecord>& runs) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* connection = connection_.get();
    common::PgTransaction transaction(connection);
    if (!transaction.active()) return false;

    // Planning has not crossed the activation barrier. A crash here leaves any
    // previously stopped run resumable and closes the incomplete replacement.
    auto interrupted = common::PgQuery(
        connection,
        "UPDATE runs SET status='interrupted',finished_at=now() WHERE status='planning'");
    if (!interrupted || PQresultStatus(interrupted.get()) != PGRES_COMMAND_OK) {
        LOG_ERROR("storage: failed to close interrupted planning runs: "
                  << (interrupted ? PQresultErrorMessage(interrupted.get())
                                  : connection_.lastError()));
        return false;
    }

    auto result = common::PgQuery(connection,
                                  "SELECT run_id,plugin_type,dataset_version,params FROM runs "
                                  "WHERE status IN ('running','stopped') ORDER BY created_at DESC");
    if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK) return false;
    std::vector<RunRecord> loaded;
    loaded.reserve(PQntuples(result.get()));
    for (int i = 0; i < PQntuples(result.get()); ++i) {
        loaded.push_back({common::PgCell(result.get(), i, 0), common::PgCell(result.get(), i, 1),
                          common::PgCell(result.get(), i, 2), common::PgCell(result.get(), i, 3)});
    }
    if (!transaction.commit()) return false;
    runs = std::move(loaded);
    return true;
}
bool Storage::SaveTask(const std::string& id, const std::string& run, const std::string& client,
                       const std::string& payload, const std::string& attempt) {
    return Exec(
        "INSERT INTO tasks(task_id,run_id,client_identity,payload,attempt_id,status) "
        "VALUES($1,$2,$3,$4,$5,'dispatched') ON CONFLICT(task_id) DO UPDATE SET "
        "client_identity=EXCLUDED.client_identity,payload=EXCLUDED.payload,"
        "attempt_id=EXCLUDED.attempt_id,status='dispatched',sent_at=now(),acked_at=NULL,reported_"
        "at=NULL",
        {{id, false}, {run, false}, {client, false}, {payload, true}, {attempt, false}});
}
bool Storage::AckTask(const std::string& id) {
    return Exec("UPDATE tasks SET status='acked',acked_at=now() WHERE task_id=$1", {{id, false}});
}
bool Storage::ReportTask(const std::string& id, const common::TaskResult& result) {
    return Exec(
        "UPDATE tasks SET status='reported',reported_at=now(),exit_code=$2,duration_ms=$3,"
        "artifact_path=$4,result=$5 WHERE task_id=$1 AND attempt_id=$6 AND run_id=$7",
        {{id, false},
         {std::to_string(result.exit_code()), false},
         {std::to_string(result.duration_ms()), false},
         {result.artifact_path(), false},
         {result.result(), true},
         {result.attempt_id(), false},
         {result.run_id(), false}});
}
bool Storage::RestoreRun(const std::string& run, const std::string& plugin) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* connection = connection_.get();
    common::PgTransaction transaction(connection);
    if (!transaction.active()) return false;

    auto reconciled = common::PgQuery(
        connection,
        "UPDATE tasks AS task SET status='reported',reported_at=COALESCE(reported_at,now()) "
        "FROM plugin_task_completions AS completion "
        "WHERE task.run_id=$1 AND completion.run_id=$1 AND completion.plugin_type=$2 "
        "AND completion.task_id=task.task_id AND task.status<>'reported'",
        {{run, false}, {plugin, false}});
    if (!reconciled || PQresultStatus(reconciled.get()) != PGRES_COMMAND_OK) {
        LOG_ERROR("storage: completion reconciliation failed: "
                  << (reconciled ? PQresultErrorMessage(reconciled.get())
                                 : connection_.lastError()));
        return false;
    }

    auto reclaimed = common::PgQuery(
        connection,
        "UPDATE tasks SET status='reclaimed' "
        "WHERE run_id=$1 AND status IS DISTINCT FROM 'reported'",
        {{run, false}});
    if (!reclaimed || PQresultStatus(reclaimed.get()) != PGRES_COMMAND_OK) {
        LOG_ERROR("storage: recovery task reclaim failed: "
                  << (reclaimed ? PQresultErrorMessage(reclaimed.get())
                                : connection_.lastError()));
        return false;
    }

    auto resumed = common::PgQuery(
        connection,
        "UPDATE runs SET status='running',finished_at=NULL "
        "WHERE run_id=$1 AND plugin_type=$2 AND status IN ('running','stopped')",
        {{run, false}, {plugin, false}});
    if (!resumed || PQresultStatus(resumed.get()) != PGRES_COMMAND_OK ||
        std::string(PQcmdTuples(resumed.get())) != "1") {
        LOG_ERROR("storage: run recovery activation failed: "
                  << (resumed ? PQresultErrorMessage(resumed.get()) : connection_.lastError()));
        return false;
    }
    return transaction.commit();
}
bool Storage::ReclaimTasks(const std::vector<std::string>& ids) {
    if (ids.empty()) return true;
    std::string array = "{";
    for (const auto& id : ids) {
        if (array.size() > 1) array += ",";
        array += '"';
        for (char c : id) {
            if (c == '\\' || c == '"') array += '\\';
            array += c;
        }
        array += '"';
    }
    array += '}';
    return Exec("UPDATE tasks SET status='reclaimed' WHERE task_id=ANY($1::text[])",
                {{array, false}});
}
}  // namespace scheduler
