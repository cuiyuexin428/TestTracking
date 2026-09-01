#pragma once

#include <string>
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

// Runs a parameterized statement (all params as $1..$n). Returns true when the
// command succeeded. On failure the connection error is logged.
bool PgExec(PGconn* conn, const char* sql, const std::vector<PgParam>& params);

// Decodes a bytea returned in the server's text format (\x<hex>).
std::string DecodeByteaText(const char* text, int length);

#endif  // _WIN32

}  // namespace common
