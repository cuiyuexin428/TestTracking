#include "common/PgUtil.h"

#include "common/Logger.h"

#ifdef _WIN32
#include <libpq-fe.h>
#endif

namespace common {

#ifdef _WIN32

bool PgExec(PGconn* conn, const char* sql, const std::vector<PgParam>& params) {
    if (conn == nullptr || sql == nullptr) {
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
    PGresult* result = params.empty() ? PQexec(conn,sql) :
        PQexecParams(conn, sql, static_cast<int>(params.size()), nullptr, values.data(),
                     lengths.data(), formats.data(), 0);
    const bool ok = result != nullptr && PQresultStatus(result) == PGRES_COMMAND_OK;
    if (!ok) {
        LOG_ERROR("pg: sql failed: " << (result ? PQresultErrorMessage(result)
                                                : PQerrorMessage(conn)));
    }
    PQclear(result);
    return ok;
}

PgResult PgQuery(PGconn* conn, const char* sql, const std::vector<PgParam>& params,
                 bool binary) {
    if (!conn || !sql) return PgResult(nullptr, PQclear);
    std::vector<const char*> values;
    std::vector<int> lengths;
    std::vector<int> formats;
    for (const auto& parameter : params) {
        values.push_back(parameter.value.data());
        lengths.push_back(static_cast<int>(parameter.value.size()));
        formats.push_back(parameter.binary ? 1 : 0);
    }
    PGresult* result = PQexecParams(conn, sql, static_cast<int>(params.size()), nullptr,
                                    values.data(), lengths.data(), formats.data(),
                                    binary ? 1 : 0);
    return PgResult(result, PQclear);
}

std::string PgCell(PGresult* result, int row, int column) {
    if (!result || PQgetisnull(result, row, column)) return {};
    return {PQgetvalue(result, row, column), static_cast<std::size_t>(PQgetlength(result, row, column))};
}

PgConnection::~PgConnection() { if (connection_) PQfinish(connection_); }
bool PgConnection::connect(const std::string& conninfo) {
    conninfo_ = conninfo;
    if (connection_) { PQfinish(connection_); connection_ = nullptr; }
    const char* keywords[]={"dbname","connect_timeout",nullptr};
    const char* values[]={conninfo.c_str(),"3",nullptr};
    connection_ = PQconnectdbParams(keywords,values,1);
    if (!connection_ || PQstatus(connection_) != CONNECTION_OK) {
        error_ = connection_ ? PQerrorMessage(connection_) : "PQconnectdb returned null";
        if (connection_) { PQfinish(connection_); connection_ = nullptr; }
        return false;
    }
    error_.clear();
    return PgExec(connection_,"SET statement_timeout='5s'; SET lock_timeout='3s'",{});
}
PGconn* PgConnection::get() {
    if ((!connection_ || PQstatus(connection_)!=CONNECTION_OK) && !conninfo_.empty()) connect(conninfo_);
    return connection_;
}

PgTransaction::PgTransaction(PGconn* connection) : connection_(connection), active_(false) {
    active_ = PgExec(connection_, "BEGIN", {});
}
PgTransaction::~PgTransaction() { if (active_) PgExec(connection_, "ROLLBACK", {}); }
bool PgTransaction::commit() {
    if (!active_) return false;
    if (!PgExec(connection_, "COMMIT", {})) return false;
    active_ = false;
    return true;
}

bool PgInitializeSchema(PGconn* connection, const char* ddl) {
    if (!connection || !ddl) return false;
    std::unique_ptr<PgTransaction> transaction;
    if (PQtransactionStatus(connection) == PQTRANS_IDLE) {
        transaction = std::make_unique<PgTransaction>(connection);
        if (!transaction->active()) return false;
    } else if (PQtransactionStatus(connection) != PQTRANS_INTRANS) {
        return false;
    }
    // CREATE TABLE IF NOT EXISTS alone is not concurrent-safe: two first-time
    // processes can race on PostgreSQL's pg_type catalog before either commits.
    // Use the same namespace for framework and plugin DDL in this database.
    auto lock = PgQuery(connection,
        "SELECT pg_advisory_xact_lock(1414810699, hashtext(current_schema()))");
    if (!lock || PQresultStatus(lock.get()) != PGRES_TUPLES_OK) {
        LOG_ERROR("pg: schema initialization lock failed: " << PQerrorMessage(connection));
        return false;
    }
    return PgExec(connection, ddl, {}) && (!transaction || transaction->commit());
}

bool EnsurePluginResultSchema(PGconn* connection) {
    const char* ddl =
        "CREATE TABLE IF NOT EXISTS plugin_runs("
        "run_id text NOT NULL, plugin_type text NOT NULL, expected_files int NOT NULL DEFAULT 0,"
        "manifest_fingerprint text, program_version text, dataset_version text,"
        "status text NOT NULL, created_at timestamptz DEFAULT now(), finished_at timestamptz,"
        "PRIMARY KEY(run_id,plugin_type));"
        "DO $$ BEGIN IF NOT EXISTS (SELECT 1 FROM pg_attribute "
        "WHERE attrelid='plugin_runs'::regclass AND attname='planned_tasks' AND NOT attisdropped) THEN "
        "ALTER TABLE plugin_runs ADD COLUMN planned_tasks int NOT NULL DEFAULT 0; END IF; END $$;"
        "CREATE TABLE IF NOT EXISTS plugin_run_files("
        "run_id text NOT NULL, plugin_type text NOT NULL, file text NOT NULL,"
        "PRIMARY KEY(run_id,plugin_type,file));"
        "DO $$ BEGIN IF NOT EXISTS (SELECT 1 FROM pg_attribute "
        "WHERE attrelid='plugin_run_files'::regclass AND attname='size_bytes' AND NOT attisdropped) THEN "
        "ALTER TABLE plugin_run_files ADD COLUMN size_bytes bigint; END IF; END $$;"
        "DO $$ BEGIN IF NOT EXISTS (SELECT 1 FROM pg_attribute "
        "WHERE attrelid='plugin_run_files'::regclass AND attname='content_sha256' AND NOT attisdropped) THEN "
        "ALTER TABLE plugin_run_files ADD COLUMN content_sha256 text; END IF; END $$;"
        "CREATE TABLE IF NOT EXISTS plugin_file_outcomes("
        "run_id text NOT NULL, plugin_type text NOT NULL, file text NOT NULL,"
        "attempt_id text NOT NULL DEFAULT '', status text NOT NULL, detail text,"
        "finished_at timestamptz DEFAULT now(), PRIMARY KEY(run_id,plugin_type,file));"
        "CREATE TABLE IF NOT EXISTS plugin_task_completions("
        "run_id text NOT NULL, plugin_type text NOT NULL, task_id text NOT NULL,"
        "exit_code int, completed_at timestamptz DEFAULT now(), PRIMARY KEY(run_id,plugin_type,task_id));"
        "DO $$ BEGIN IF NOT EXISTS (SELECT 1 FROM pg_attribute "
        "WHERE attrelid='plugin_task_completions'::regclass AND attname='exit_code' AND NOT attisdropped) THEN "
        "ALTER TABLE plugin_task_completions ADD COLUMN exit_code int; END IF; END $$;";
    return PgInitializeSchema(connection, ddl);
}

bool LockFileOutcome(PGconn* connection, const std::string& plugin,
                     const std::string& run, const std::string& file, bool* successful) {
    if (!successful) return false;
    if (!PgExec(connection,
        "INSERT INTO plugin_file_outcomes(run_id,plugin_type,file,status) VALUES($1,$2,$3,'pending') "
        "ON CONFLICT DO NOTHING",{{run,false},{plugin,false},{file,false}})) return false;
    auto result = PgQuery(connection,
        "SELECT status FROM plugin_file_outcomes WHERE run_id=$1 AND plugin_type=$2 AND file=$3 FOR UPDATE",
        {{run,false},{plugin,false},{file,false}});
    if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK) return false;
    if (PQntuples(result.get()) == 0) { *successful = false; return true; }
    *successful = PgCell(result.get(),0,0) == "success";
    return true;
}

bool SaveFileOutcome(PGconn* connection, const std::string& plugin,
                     const std::string& run, const std::string& file,
                     const std::string& attempt, bool successful, const std::string& detail) {
    const char* sql =
        "INSERT INTO plugin_file_outcomes(run_id,plugin_type,file,attempt_id,status,detail,finished_at) "
        "VALUES($1,$2,$3,$4,$5,$6,now()) ON CONFLICT(run_id,plugin_type,file) DO UPDATE SET "
        "attempt_id=EXCLUDED.attempt_id,status=EXCLUDED.status,detail=EXCLUDED.detail,finished_at=now() "
        "WHERE plugin_file_outcomes.status <> 'success'";
    return PgExec(connection, sql, {{run,false},{plugin,false},{file,false},{attempt,false},
                                    {successful ? "success" : "failed",false},{detail,false}});
}

#endif  // _WIN32

}  // namespace common
