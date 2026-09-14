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
#include <atomic>
#include <memory>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#else
#include <unistd.h>
#include <poll.h>
#endif

namespace {

// The input worker owns only independent signals, never a Client reference.
// Reads are polled so startup failure can stop and join the worker immediately.
class InputMonitor {
public:
    explicit InputMonitor(std::shared_ptr<std::atomic<bool>> stop_signal)
        : stop_signal_(std::move(stop_signal)), worker_([this] { Run(); }) {}
    ~InputMonitor() {
        finished_.store(true);
        worker_.join();
    }

private:
    enum class ReadResult { Pending, Character, End };
    ReadResult ReadCharacter(char& character) {
#ifdef _WIN32
        const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        if (GetConsoleMode(input, &mode)) {
            if (!_kbhit()) return ReadResult::Pending;
            const int key = _getch();
            if (key == 0 || key == 224) {
                if (_kbhit()) (void)_getch();
                return ReadResult::Pending;
            }
            character = static_cast<char>(key);
            return ReadResult::Character;
        }
        if (GetFileType(input) == FILE_TYPE_PIPE) {
            DWORD available = 0;
            if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) return ReadResult::End;
            if (available == 0) return ReadResult::Pending;
        }
        DWORD read = 0;
        return ReadFile(input, &character, 1, &read, nullptr) && read == 1
            ? ReadResult::Character : ReadResult::End;
#else
        pollfd input{STDIN_FILENO, POLLIN, 0};
        if (poll(&input, 1, 0) <= 0) return ReadResult::Pending;
        return read(STDIN_FILENO, &character, 1) == 1 ? ReadResult::Character : ReadResult::End;
#endif
    }

    void Run() {
        std::string line;
        while (!finished_.load()) {
            char character = 0;
            const auto read = ReadCharacter(character);
            if (read == ReadResult::End) {
                stop_signal_->store(true);
                return;
            }
            if (read == ReadResult::Pending) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            if (character == '\n' || character == '\r') {
                if (line == "quit" || line == "exit") {
                    stop_signal_->store(true);
                    return;
                }
                line.clear();
            } else if (character == '\b') {
                if (!line.empty()) line.pop_back();
            } else if (line.size() < 1024) {
                line += character;
            }
        }
    }

    std::shared_ptr<std::atomic<bool>> stop_signal_;
    std::atomic<bool> finished_{false};
    std::thread worker_;
};

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

        auto stop_signal = std::make_shared<std::atomic<bool>>(false);
        client::Client client(config, stop_signal);
        InputMonitor input(stop_signal);
        LOG_INFO("client started, config=" << config_path);
        client.start();
        LOG_INFO("client stopped");
    } catch (const std::exception& e) {
        LOG_ERROR("client failed: " << e.what());
        PauseIfInteractive();
        return 1;
    }
    return 0;
}
