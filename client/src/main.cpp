#include "client/Client.h"

#include "common/Config.h"
#include "common/Logger.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

const char* kDefaultConfigPath = "config/client.json";

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

void PauseIfInteractive() {
#ifdef _WIN32
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (input != INVALID_HANDLE_VALUE && GetConsoleMode(input, &mode)) {
        std::system("pause");
    }
#else
    if (isatty(STDIN_FILENO)) {
        std::system("read -n 1 -s -r -p \"Press any key to continue...\"");
    }
#endif
}

std::string ResolveConfigPath(int argc, char* argv[]) {
    if (argc > 1) {
        return argv[1];
    }
    if (FileExists(kDefaultConfigPath)) {
        return kDefaultConfigPath;
    }
    const std::string next_to_exe = ExeDirectory(argv[0]) + "/config/client.json";
    if (FileExists(next_to_exe)) {
        return next_to_exe;
    }
    return kDefaultConfigPath;
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::string config_path = ResolveConfigPath(argc, argv);

    try {
        common::Config& config = common::Config::instance();
        config.load(config_path);
        common::Logger::instance().setLevel(common::Logger::Level::Info);

        client::Client client(config);
        // The main thread runs the state machine; a stdin thread provides
        // the quit control.
        std::thread stdin_thread([&client] {
            std::string line;
            while (std::getline(std::cin, line)) {
                if (line == "quit" || line == "exit") {
                    client.stop();
                    break;
                }
            }
            client.stop();  // stdin EOF also stops the client
        });


        LOG_INFO("client started, config=" << config_path);
        try {
            client.start();  // blocks: INIT + state machine until stop()
        } catch (...) {
            if (stdin_thread.joinable()) {
                stdin_thread.detach();
            }
            throw;
        }
        if (stdin_thread.joinable()) {
            stdin_thread.join();
        }
        LOG_INFO("client stopped");
    } catch (const std::exception& e) {
        LOG_ERROR("client failed: " << e.what());
        PauseIfInteractive();
        return 1;
    }
    return 0;
}
