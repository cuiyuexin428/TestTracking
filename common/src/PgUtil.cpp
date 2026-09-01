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
    PGresult* result =
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

std::string DecodeByteaText(const char* text, int length) {
    // Server text format for bytea: \x<hex>.
    if (text == nullptr || length < 2 || text[0] != '\\' || text[1] != 'x') {
        return std::string();
    }
    const std::string hex(text + 2, static_cast<size_t>(length - 2));
    std::string out;
    out.reserve(hex.size() / 2);
    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<char>((hexval(hex[i]) << 4) | hexval(hex[i + 1])));
    }
    return out;
}

#endif  // _WIN32

}  // namespace common
