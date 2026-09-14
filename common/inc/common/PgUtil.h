#pragma once

#include <string>
#include <memory>
#include <vector>

#ifdef _WIN32
#include <libpq-fe.h>
#endif

namespace common {

#ifdef _WIN32

// A parameterized-query parameter. binary=true sends a bytea in binary format.
struct PgParam {
    std::string value;
    bool binary = false;
};

using PgResult = std::unique_ptr<PGresult, decltype(&PQclear)>;
PgResult PgQuery(PGconn* conn, const char* sql, const std::vector<PgParam>& params = {},
                 bool binary = false);
std::string PgCell(PGresult* result, int row, int column);

// Recovery happens before an operation. Statements are never silently replayed
// after an ambiguous connection loss, particularly inside a transaction.
class PgConnection {
public:
    ~PgConnection();
    bool connect(const std::string& conninfo);
    PGconn* get();
    const std::string& lastError() const { return error_; }
private:
    PGconn* connection_ = nullptr;
    std::string conninfo_, error_;
};

class PgTransaction {
public:
    explicit PgTransaction(PGconn* connection);
    ~PgTransaction();
    bool active() const { return active_; }
    bool commit();
private:
    PGconn* connection_;
    bool active_;
};

bool EnsurePluginResultSchema(PGconn* connection);
// First successful publication wins. Call inside the same transaction as raw
// writes; the row lock serializes concurrent attempts for the file.
bool LockFileOutcome(PGconn* connection, const std::string& plugin,
                     const std::string& run, const std::string& file, bool* successful);
bool SaveFileOutcome(PGconn* connection, const std::string& plugin,
                     const std::string& run, const std::string& file,
                     const std::string& attempt, bool successful, const std::string& detail);

// Runs a parameterized statement (all params as $1..$n). Returns true when the
// command succeeded. On failure the connection error is logged.
bool PgExec(PGconn* conn, const char* sql, const std::vector<PgParam>& params);

#endif  // _WIN32

}  // namespace common
