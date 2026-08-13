// Unit test: Config loading and typed access.
#include "common/Config.h"

#include <cstdio>
#include <exception>
#include <fstream>
#include <string>

namespace {

bool WriteConfig() {
    std::ofstream out("config_test_tmp.json");
    if (!out.is_open()) {
        return false;
    }
    out << R"({
  "server": { "port": 5555, "host": "127.0.0.1" },
  "heartbeat_interval_ms": 1000,
  "flag": true,
  "name": "scheduler"
})";
    return out.good();
}

int Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        return 1;
    }
    return 0;
}

}  // namespace

int main() {
    if (!WriteConfig()) {
        std::fprintf(stderr, "FAIL: cannot write config file\n");
        return 1;
    }

    try {
        common::Config& cfg = common::Config::instance();
        cfg.load("config_test_tmp.json");

        int rc = 0;
        rc |= Check(cfg.get("server.port", -1) == 5555, "nested int");
        rc |= Check(cfg.get("server.host", std::string()) == "127.0.0.1", "nested string");
        rc |= Check(cfg.get("heartbeat_interval_ms", 0) == 1000, "top-level int");
        rc |= Check(cfg.get("flag", false), "top-level bool");
        rc |= Check(cfg.get("missing.key", 42) == 42, "missing key returns default");

        int port = 0;
        rc |= Check(cfg.get("server.port", port) && port == 5555, "get into reference");

        std::string name;
        rc |= Check(cfg.get("name", name) && name == "scheduler", "string into reference");

        int bad = 0;
        rc |= Check(!cfg.get("name", bad), "type mismatch returns false");

        if (rc == 0) {
            std::printf("PASS: Config\n");
        }
        return rc;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
