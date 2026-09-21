#include "scheduler/Storage.h"

#include <utility>
#include <json.hpp>

#include "common/Logger.h"

namespace scheduler {
namespace {
bool ParseWorkerIds(const std::string& encoded, std::vector<std::string>& worker_ids) {
    const auto value = nlohmann::json::parse(encoded, nullptr, false);
    if (!value.is_array()) return false;
    for (const auto& item : value) {
        if (!item.is_string() || item.get_ref<const std::string&>().empty()) return false;
        worker_ids.push_back(item.get<std::string>());
    }
    return true;
}
}  // namespace

bool Storage::init(const std::string& conninfo) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connection_.connect(conninfo)) return false;
    const char* ddl =
        "CREATE TABLE IF NOT EXISTS runs ("
        " run_id text PRIMARY KEY, plugin_type text, dataset_version text, params text,"
        " status text NOT NULL DEFAULT 'running', created_at timestamptz DEFAULT now(), "
        "finished_at timestamptz, work_fingerprint text,"
        "force_rerun boolean NOT NULL DEFAULT false, force_rerun_reason text, rerun_of text,"
        "worker_ids jsonb NOT NULL DEFAULT '[]'::jsonb, execution_fingerprint text);"
        "DO $$ BEGIN IF NOT EXISTS (SELECT 1 FROM pg_attribute "
        "WHERE attrelid='runs'::regclass AND attname='work_fingerprint' AND NOT attisdropped) THEN "
        "ALTER TABLE runs ADD COLUMN work_fingerprint text; END IF; END $$;"
        "DO $$ BEGIN IF NOT EXISTS (SELECT 1 FROM pg_attribute "
        "WHERE attrelid='runs'::regclass AND attname='force_rerun' AND NOT attisdropped) THEN "
        "ALTER TABLE runs ADD COLUMN force_rerun boolean NOT NULL DEFAULT false; END IF; END $$;"
        "DO $$ BEGIN IF NOT EXISTS (SELECT 1 FROM pg_attribute "
        "WHERE attrelid='runs'::regclass AND attname='force_rerun_reason' AND NOT attisdropped) THEN "
        "ALTER TABLE runs ADD COLUMN force_rerun_reason text; END IF; END $$;"
        "DO $$ BEGIN IF NOT EXISTS (SELECT 1 FROM pg_attribute "
        "WHERE attrelid='runs'::regclass AND attname='rerun_of' AND NOT attisdropped) THEN "
        "ALTER TABLE runs ADD COLUMN rerun_of text; END IF; END $$;"
        "DO $$ BEGIN IF NOT EXISTS (SELECT 1 FROM pg_attribute "
        "WHERE attrelid='runs'::regclass AND attname='worker_ids' AND NOT attisdropped) THEN "
        "ALTER TABLE runs ADD COLUMN worker_ids jsonb NOT NULL DEFAULT '[]'::jsonb; END IF; END $$;"
        "DO $$ BEGIN IF NOT EXISTS (SELECT 1 FROM pg_attribute "
        "WHERE attrelid='runs'::regclass AND attname='execution_fingerprint' AND NOT attisdropped) THEN "
        "ALTER TABLE runs ADD COLUMN execution_fingerprint text; END IF; END $$;"
        "CREATE TABLE IF NOT EXISTS tasks ("
        " task_id text PRIMARY KEY, run_id text, client_identity text, status text,"
        " sent_at timestamptz DEFAULT now(), acked_at timestamptz, reported_at timestamptz,"
        " exit_code int, duration_ms bigint, artifact_path text, result bytea, payload bytea);"
        "DO $$ BEGIN IF NOT EXISTS (SELECT 1 FROM pg_attribute "
        "WHERE attrelid='tasks'::regclass AND attname='attempt_id' AND NOT attisdropped) THEN "
        "ALTER TABLE tasks ADD COLUMN attempt_id text NOT NULL DEFAULT ''; END IF; END $$;"
        "CREATE TABLE IF NOT EXISTS plugin_task_completions("
        " run_id text NOT NULL, plugin_type text NOT NULL, task_id text NOT NULL,"
        " exit_code int, completed_at timestamptz DEFAULT now(), PRIMARY KEY(run_id,plugin_type,task_id));"
        "DO $$ BEGIN IF NOT EXISTS (SELECT 1 FROM pg_attribute "
        "WHERE attrelid='plugin_task_completions'::regclass AND attname='exit_code' AND NOT attisdropped) THEN "
        "ALTER TABLE plugin_task_completions ADD COLUMN exit_code int; END IF; END $$;"
        "CREATE INDEX IF NOT EXISTS idx_tasks_run ON tasks(run_id);"
        "DO $$ BEGIN IF EXISTS (SELECT 1 FROM pg_class WHERE oid='runs'::regclass "
        "AND relkind IN ('r','p')) THEN CREATE INDEX IF NOT EXISTS "
        "idx_runs_work_fingerprint ON runs(work_fingerprint); END IF; END $$;";
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
Storage::EnqueueResult Storage::EnqueueRun(const RunRecord& run) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* connection = connection_.get();
    common::PgTransaction transaction(connection);
    if (!transaction.active()) return {};

    auto advisory = common::PgQuery(
        connection, "SELECT pg_advisory_xact_lock(hashtext($1))",
        {{run.work_fingerprint, false}});
    if (!advisory || PQresultStatus(advisory.get()) != PGRES_TUPLES_OK) {
        LOG_ERROR("storage: failed to lock work fingerprint");
        return {};
    }

    auto existing = common::PgQuery(
        connection,
        "SELECT r.run_id,r.status FROM runs r WHERE r.work_fingerprint=$1 "
        "AND r.status IN ('queued','planning','running','stopped','finished') "
        // Finished means every outcome was received, including business failures.
        // Ordinary submissions only reuse fully successful work; explicit reruns
        // still link to the latest attempt, even when its outcomes failed.
        "AND ($2::boolean OR r.status<>'finished' OR "
        "(EXISTS (SELECT 1 FROM tasks t WHERE t.run_id=r.run_id) AND NOT EXISTS "
        "(SELECT 1 FROM tasks t WHERE t.run_id=r.run_id "
        "AND (t.status IS DISTINCT FROM 'reported' OR t.exit_code IS DISTINCT FROM 0)))) "
        "ORDER BY r.created_at DESC,r.run_id DESC LIMIT 1",
        {{run.work_fingerprint, false}, {run.force_rerun ? "true" : "false", false}});
    if (!existing || PQresultStatus(existing.get()) != PGRES_TUPLES_OK) {
        LOG_ERROR("storage: failed to check duplicate work");
        return {};
    }
    const bool has_existing = PQntuples(existing.get()) != 0;
    const std::string existing_id = has_existing ? common::PgCell(existing.get(), 0, 0) : "";
    const std::string existing_status = has_existing ? common::PgCell(existing.get(), 0, 1) : "";
    if (has_existing && !run.force_rerun) {
        if (!transaction.commit()) return {};
        return {EnqueueResult::Outcome::Duplicate, existing_id, existing_status, {}};
    }
    if (run.force_rerun && !has_existing) {
        if (!transaction.commit()) return {};
        return {EnqueueResult::Outcome::ForceTargetMissing, {}, {}, {}};
    }

    const std::string rerun_of = run.force_rerun ? existing_id : std::string();
    auto inserted = common::PgQuery(
        connection,
        "INSERT INTO runs(run_id,plugin_type,dataset_version,params,status,work_fingerprint,"
        "force_rerun,force_rerun_reason,rerun_of,worker_ids,execution_fingerprint) "
        "VALUES($1,$2,$3,$4,'queued',$5,$6::boolean,NULLIF($7,''),NULLIF($8,''),$9::jsonb,NULLIF($10,''))",
        {{run.run_id, false}, {run.plugin_type, false}, {run.dataset_version, false},
         {run.params, false}, {run.work_fingerprint, false},
         {run.force_rerun ? "true" : "false", false}, {run.force_rerun_reason, false},
         {rerun_of, false}, {nlohmann::json(run.worker_ids).dump(), false},
         {run.execution_fingerprint, false}});
    if (!inserted || PQresultStatus(inserted.get()) != PGRES_COMMAND_OK ||
        std::string(PQcmdTuples(inserted.get())) != "1") {
        LOG_ERROR("storage: failed to enqueue run: "
                  << (inserted ? PQresultErrorMessage(inserted.get()) : connection_.lastError()));
        return {};
    }
    if (!transaction.commit()) return {};
    return {EnqueueResult::Outcome::Created, run.run_id, "queued", rerun_of};
}
bool Storage::BeginPlanning(const std::string& id, const std::string& plugin) {
    return Exec("UPDATE runs SET status='planning',finished_at=NULL "
                "WHERE run_id=$1 AND plugin_type=$2 AND status='queued'",
                {{id, false}, {plugin, false}});
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
bool Storage::CancelQueuedRun(const std::string& id, const std::string& plugin) {
    return Exec("UPDATE runs SET status='cancelled',finished_at=now() "
                "WHERE run_id=$1 AND plugin_type=$2 AND status='queued'",
                {{id, false}, {plugin, false}});
}
bool Storage::LoadUnfinishedRuns(std::vector<RunRecord>& runs,
                                 std::vector<RunRecord>& queued_runs) {
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

    const char* columns =
        "run_id,plugin_type,dataset_version,params,COALESCE(work_fingerprint,''),"
        "force_rerun,COALESCE(force_rerun_reason,''),COALESCE(rerun_of,''),status,worker_ids::text,"
        "COALESCE(execution_fingerprint,'')";
    auto result = common::PgQuery(
        connection, (std::string("SELECT ") + columns + " FROM runs "
                     "WHERE status IN ('running','stopped') ORDER BY created_at DESC,run_id DESC")
                        .c_str());
    if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK) return false;
    std::vector<RunRecord> loaded;
    loaded.reserve(PQntuples(result.get()));
    for (int i = 0; i < PQntuples(result.get()); ++i) {
        loaded.push_back({common::PgCell(result.get(), i, 0), common::PgCell(result.get(), i, 1),
                          common::PgCell(result.get(), i, 2), common::PgCell(result.get(), i, 3),
                          common::PgCell(result.get(), i, 4),
                          common::PgCell(result.get(), i, 5) == "t",
                          common::PgCell(result.get(), i, 6), common::PgCell(result.get(), i, 7),
                          common::PgCell(result.get(), i, 8)});
        if (!ParseWorkerIds(common::PgCell(result.get(), i, 9), loaded.back().worker_ids)) {
            LOG_ERROR("storage: invalid saved worker_ids for run " << loaded.back().run_id);
            return false;
        }
        loaded.back().execution_fingerprint = common::PgCell(result.get(), i, 10);
    }

    auto queued = common::PgQuery(
        connection, (std::string("SELECT ") + columns + " FROM runs "
                     "WHERE status='queued' ORDER BY created_at,run_id")
                        .c_str());
    if (!queued || PQresultStatus(queued.get()) != PGRES_TUPLES_OK) return false;
    std::vector<RunRecord> loaded_queued;
    loaded_queued.reserve(PQntuples(queued.get()));
    for (int i = 0; i < PQntuples(queued.get()); ++i) {
        loaded_queued.push_back(
            {common::PgCell(queued.get(), i, 0), common::PgCell(queued.get(), i, 1),
             common::PgCell(queued.get(), i, 2), common::PgCell(queued.get(), i, 3),
             common::PgCell(queued.get(), i, 4), common::PgCell(queued.get(), i, 5) == "t",
             common::PgCell(queued.get(), i, 6), common::PgCell(queued.get(), i, 7),
             common::PgCell(queued.get(), i, 8)});
        if (!ParseWorkerIds(common::PgCell(queued.get(), i, 9), loaded_queued.back().worker_ids)) {
            LOG_ERROR("storage: invalid saved worker_ids for queued run " << loaded_queued.back().run_id);
            return false;
        }
        loaded_queued.back().execution_fingerprint = common::PgCell(queued.get(), i, 10);
    }
    if (!transaction.commit()) return false;
    runs = std::move(loaded);
    queued_runs = std::move(loaded_queued);
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
        "UPDATE tasks AS task SET status='reported',reported_at=COALESCE(reported_at,now()),"
        "exit_code=COALESCE(completion.exit_code,task.exit_code) "
        "FROM plugin_task_completions AS completion "
        "WHERE task.run_id=$1 AND completion.run_id=$1 AND completion.plugin_type=$2 "
        "AND completion.task_id=task.task_id "
        "AND (task.status<>'reported' OR task.exit_code IS NULL)",
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
