#include "scheduler/Storage.h"

#include "common/Logger.h"

#include <utility>

namespace scheduler {

Storage::~Storage() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (conn_ != nullptr) {
        PQfinish(conn_);
        conn_ = nullptr;
    }
}

bool Storage::init(const std::string& conninfo) {
    std::lock_guard<std::mutex> lock(mutex_);
    conn_ = PQconnectdb(conninfo.c_str());
    if (conn_ == nullptr || PQstatus(conn_) != CONNECTION_OK) {
        LOG_ERROR("storage: postgres connect failed: "
                  << (conn_ ? PQerrorMessage(conn_) : "PQconnectdb returned null"));
        if (conn_ != nullptr) {
            PQfinish(conn_);
            conn_ = nullptr;
        }
        return false;
    }

    const char* ddl =
        "CREATE TABLE IF NOT EXISTS runs ("
        " run_id text PRIMARY KEY, plugin_type text, dataset_version text, params text,"
        " status text NOT NULL DEFAULT 'running',"
        " created_at timestamptz DEFAULT now(), finished_at timestamptz);"
        "CREATE TABLE IF NOT EXISTS tasks ("
        " task_id text PRIMARY KEY, run_id text, client_identity text, status text,"
        " sent_at timestamptz DEFAULT now(), acked_at timestamptz, reported_at timestamptz,"
        " exit_code int, duration_ms bigint, artifact_path text, result bytea, payload bytea);"
        "CREATE INDEX IF NOT EXISTS idx_tasks_run ON tasks(run_id);";
    PGresult* result = PQexec(conn_, ddl);
    const bool ok = result != nullptr && PQresultStatus(result) == PGRES_COMMAND_OK;
    if (!ok) {
        LOG_ERROR("storage: schema init failed: "
                  << (result ? PQresultErrorMessage(result) : PQerrorMessage(conn_)));
    }
    PQclear(result);
    return ok;
}

bool Storage::Exec(const char* sql, const std::vector<PgParam>& params) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (conn_ == nullptr) {
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
        PQexecParams(conn_, sql, static_cast<int>(params.size()), nullptr, values.data(),
                     lengths.data(), formats.data(), 0);
    const bool ok = result != nullptr && PQresultStatus(result) == PGRES_COMMAND_OK;
    if (!ok) {
        LOG_ERROR("storage: sql failed: " << (result ? PQresultErrorMessage(result)
                                                     : PQerrorMessage(conn_)));
    }
    PQclear(result);
    return ok;
}

void Storage::SaveRun(const std::string& run_id, const std::string& plugin_type,
                      const std::string& dataset_version, const std::string& params) {
    Exec("INSERT INTO runs(run_id, plugin_type, dataset_version, params, status) "
         "VALUES($1,$2,$3,$4,'running') "
         "ON CONFLICT (run_id) DO UPDATE SET plugin_type=EXCLUDED.plugin_type, "
         "dataset_version=EXCLUDED.dataset_version, params=EXCLUDED.params, "
         "status='running', finished_at=NULL",
         {{run_id, false}, {plugin_type, false}, {dataset_version, false},
          {params, false}});
}

void Storage::FinishRun(const std::string& run_id, const std::string& status) {
    Exec("UPDATE runs SET status=$2, finished_at=now() WHERE run_id=$1",
         {{run_id, false}, {status, false}});
}

std::vector<std::string> Storage::LoadUnfinishedRunIds() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> ids;
    if (conn_ == nullptr) {
        return ids;
    }
    PGresult* result = PQexec(conn_, "SELECT run_id FROM runs WHERE status='running'");
    if (result != nullptr && PQresultStatus(result) == PGRES_TUPLES_OK) {
        for (int i = 0; i < PQntuples(result); ++i) {
            ids.emplace_back(PQgetvalue(result, i, 0));
        }
    }
    PQclear(result);
    return ids;
}

void Storage::SaveTask(const std::string& task_id, const std::string& run_id,
                       const std::string& client_identity, const std::string& payload) {
    Exec("INSERT INTO tasks(task_id, run_id, client_identity, status, payload) "
         "VALUES($1,$2,$3,'dispatched',$4) "
         "ON CONFLICT (task_id) DO UPDATE SET client_identity=EXCLUDED.client_identity, "
         "status='dispatched', sent_at=now(), payload=EXCLUDED.payload",
         {{task_id, false}, {run_id, false}, {client_identity, false},
          {payload, true}});
}

void Storage::AckTask(const std::string& task_id) {
    Exec("UPDATE tasks SET status='acked', acked_at=now() WHERE task_id=$1",
         {{task_id, false}});
}

void Storage::ReportTask(const std::string& task_id, const common::TaskResult& result) {
    const std::string& result_bytes = result.result();
    Exec("UPDATE tasks SET status='reported', reported_at=now(), exit_code=$2, "
         "duration_ms=$3, artifact_path=$4, result=$5 WHERE task_id=$1",
         {{task_id, false},
          {std::to_string(result.exit_code()), false},
          {std::to_string(result.duration_ms()), false},
          {result.artifact_path(), false},
          {result_bytes, true}});
}

}  // namespace scheduler
