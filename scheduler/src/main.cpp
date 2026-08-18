#include "common/Config.h"
#include "common/Logger.h"
#include "common/TaskPlugin.h"
#include "scheduler/Scheduler.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

const char* kDefaultConfigPath = "config/scheduler.json";

std::string ExeDirectory(const char* argv0) {
#ifdef _WIN32
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    const std::string path(buf);
    const size_t pos = path.find_last_of("\\/");
    return pos == std::string::npos ? "." : path.substr(0, pos);
#else
    char buf[4096] = {};
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        const std::string path(buf);
        const size_t pos = path.find_last_of('/');
        return pos == std::string::npos ? "." : path.substr(0, pos);
    }
    const std::string path(argv0 != nullptr ? argv0 : "");
    const size_t pos = path.find_last_of('/');
    return pos == std::string::npos ? "." : path.substr(0, pos);
#endif
}

bool FileExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
}

std::string ResolveConfigPath(int argc, char* argv[]) {
    if (argc > 1) {
        return argv[1];
    }
    // Unified convention: config files live in a "config" directory.
    if (FileExists(kDefaultConfigPath)) {  // relative to the working directory
        return kDefaultConfigPath;
    }
    const std::string next_to_exe = ExeDirectory(argv[0]) + "/config/scheduler.json";
    if (FileExists(next_to_exe)) {
        return next_to_exe;
    }
    return kDefaultConfigPath;
}

void PrintHelp() {
    std::printf(
        "commands:\n"
        "  help                                     show this help\n"
        "  run <plugin_type> <dataset_version> [params]  start a run\n"
        "  stop <plugin_type>                          stop the active run\n"
        "  wakeup [plugin_type]                        resume stopped clients\n"
        "  clients                                  list connected clients\n"
        "  status                                   plugins and in-flight tasks\n"
        "  quit                                     stop the scheduler\n");
}

void PrintClients(const std::vector<common::ClientInfo>& clients) {
    if (clients.empty()) {
        std::printf("no clients connected\n");
        return;
    }
    for (const common::ClientInfo& c : clients) {
        const common::Heartbeat& hb = c.heartbeat;
        const char* status =
            hb.status() == common::Heartbeat::BUSY ? "busy" : "idle";
        std::printf("client %s type=%s ip=%s peer=%s status=%s registered=%s",
                    c.identity.c_str(), c.plugin_type.c_str(), hb.ip().c_str(),
                    c.peer_address.c_str(), status, c.registered ? "yes" : "no");
        if (hb.has_resource()) {
            std::printf(" mem=%.0fMB disk=%.1fGB cpu=%d%%",
                        hb.resource().memory_bytes() / (1024.0 * 1024.0),
                        hb.resource().disk_free_bytes() / (1024.0 * 1024.0 * 1024.0),
                        hb.resource().cpu_percent());
        }
        std::printf(" seen=%.1fs info=%s\n", c.last_seen_age_ms / 1000.0,
                    hb.plugin_info().c_str());
    }
}

std::string TrimLeft(const std::string& s) {
    const size_t pos = s.find_first_not_of(" \t");
    return pos == std::string::npos ? std::string() : s.substr(pos);
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::string config_path = ResolveConfigPath(argc, argv);

    try {
        common::Config& config = common::Config::instance();
        config.load(config_path);
        common::Logger::instance().setLevel(common::Logger::Level::Info);

        scheduler::Scheduler sched(config);
        sched.start();
        LOG_INFO("scheduler started, config=" << config_path);
        PrintHelp();

        std::string line;
        while (std::getline(std::cin, line)) {
            if (line == "quit" || line == "exit") {
                break;
            }
            if (line == "help") {
                PrintHelp();
            } else if (line == "clients") {
                PrintClients(sched.clients());
            } else if (line == "wakeup" || line.rfind("wakeup ", 0) == 0) {
                std::istringstream in(line);
                std::string cmd;
                std::string plugin_type;
                in >> cmd >> plugin_type;
                sched.wakeupClients(plugin_type);
            } else if (line == "status") {
                std::printf("plugins=%zu in_flight=%zu\n", sched.pluginCount(),
                            sched.inFlightCount());
            } else if (line.rfind("run ", 0) == 0) {
                std::istringstream in(line.substr(4));
                std::string plugin_type;
                std::string dataset_version;
                in >> plugin_type >> dataset_version;
                std::string params;
                std::getline(in, params);
                params = TrimLeft(params);
                if (plugin_type.empty() || dataset_version.empty()) {
                    LOG_WARN("usage: run <plugin_type> <dataset_version> [params]");
                } else {
                    sched.createRun(plugin_type, dataset_version, params);
                }
            } else if (line.rfind("stop ", 0) == 0) {
                std::istringstream in(line.substr(5));
                std::string plugin_type;
                in >> plugin_type;
                if (plugin_type.empty()) {
                    LOG_WARN("usage: stop <plugin_type>");
                } else {
                    sched.stopRun(plugin_type);
                }
            } else {
                LOG_WARN("unknown command: " << line);
            }
        }

        sched.stop();
        LOG_INFO("scheduler stopped");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "scheduler failed: %s\n", e.what());
        return 1;
    }
    return 0;
}
