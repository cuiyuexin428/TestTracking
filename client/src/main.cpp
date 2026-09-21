#include "client/Client.h"
#include "client/ProcessAgent.h"

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
#include <sstream>
#include <vector>

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

struct LaunchOptions {
    std::string config_path;
    std::string worker_plugin;
    bool qualify_worker_id = false;
#ifdef _WIN32
    HANDLE stop_event = nullptr;
    HANDLE drain_event = nullptr;
    HANDLE ready_event = nullptr;
#endif
};

std::string ExeDirectory(const char* argv0) {
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return length && length < buffer.size()
        ? std::filesystem::path(std::wstring(buffer.data(), length)).parent_path().u8string() : ".";
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
    return std::filesystem::exists(std::filesystem::u8path(path), ec) && !ec;
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

LaunchOptions ParseArguments(int argc, char* argv[]) {
    LaunchOptions result;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto value = [&](const char* name) -> std::string {
            if (++index >= argc) throw std::runtime_error(std::string(name) + " requires a value");
            return argv[index];
        };
        if (argument == "--config") result.config_path = value("--config");
        else if (argument == "--worker-plugin") result.worker_plugin = value("--worker-plugin");
        else if (argument == "--qualify-worker-id") result.qualify_worker_id = true;
#ifdef _WIN32
        else if (argument == "--stop-event" || argument == "--drain-event" || argument == "--ready-event") {
            const auto raw = std::stoull(value(argument.c_str()));
            auto handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(raw));
            if (argument == "--stop-event") result.stop_event = handle;
            else if (argument == "--drain-event") result.drain_event = handle;
            else result.ready_event = handle;
        }
#endif
        else if (!argument.empty() && argument[0] != '-' && result.config_path.empty())
            result.config_path = argument;
        else
            throw std::runtime_error("unknown client argument: " + argument);
    }
    if (result.config_path.empty()) result.config_path = ResolveConfigPath(argc, argv);
    return result;
}

#ifdef _WIN32

std::filesystem::path ExecutablePath() {
    std::vector<wchar_t> buffer(32768);
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0 || size >= buffer.size()) throw std::runtime_error("cannot resolve client executable path");
    return std::filesystem::path(std::wstring(buffer.data(), size));
}

class StopEventMonitor {
public:
    StopEventMonitor(HANDLE event, std::shared_ptr<std::atomic<bool>> stop)
        : event_(event), stop_(std::move(stop)) {
        if (event_) worker_ = std::thread([this] { Run(); });
    }
    ~StopEventMonitor() {
        finished_.store(true);
        if (worker_.joinable()) worker_.join();
        if (event_) CloseHandle(event_);
    }

private:
    void Run() {
        while (!finished_.load()) {
            if (WaitForSingleObject(event_, 50) == WAIT_OBJECT_0) {
                stop_->store(true);
                return;
            }
        }
    }
    HANDLE event_ = nullptr;
    std::shared_ptr<std::atomic<bool>> stop_;
    std::atomic<bool> finished_{false};
    std::thread worker_;
};


#endif

}  // namespace

int RunMain(int argc, char* argv[]) {
    try {
        auto options = ParseArguments(argc, argv);
        options.config_path = std::filesystem::absolute(std::filesystem::u8path(options.config_path)).u8string();
        common::Config& config = common::Config::instance();
        config.load(options.config_path);
        common::Logger::instance().setLevel(common::Logger::Level::Info);

        auto stop_signal = std::make_shared<std::atomic<bool>>(false);
        if (!options.worker_plugin.empty()) {
#ifdef _WIN32
            StopEventMonitor monitor(options.stop_event, stop_signal);
            auto drain_signal = std::make_shared<std::atomic<bool>>(false);
            StopEventMonitor drain_monitor(options.drain_event, drain_signal);
#endif
            LOG_INFO("client worker started, plugin=" << options.worker_plugin
                                                        << " config=" << options.config_path);
            client::Client worker(config, stop_signal, options.worker_plugin,
                                  options.qualify_worker_id
#ifdef _WIN32
                                  , drain_signal, [event = options.ready_event] { if (event) SetEvent(event); }
#endif
                                  );
            worker.start();
#ifdef _WIN32
            if (options.ready_event) CloseHandle(options.ready_event);
#endif
            LOG_INFO("client worker stopped: plugin=" << options.worker_plugin);
            return 0;
        }

        InputMonitor input(stop_signal);
        LOG_INFO("client agent started, config=" << options.config_path);
        std::vector<std::string> enabled_plugins;
        if (const auto* plugins = config.find("plugins"); plugins && plugins->is_object()) {
            for (const auto& item : plugins->items()) {
                if (item.value().is_object() && item.value().value("enabled", false))
                    enabled_plugins.push_back(item.key());
            }
        }
        if (enabled_plugins.empty()) throw std::runtime_error("Client: no enabled plugin");
#ifdef _WIN32
        const int result = client::RunProcessAgent(ExecutablePath(), std::filesystem::u8path(options.config_path), stop_signal);
        if (result != 0) return result;
#else
        if (enabled_plugins.size() != 1)
            throw std::runtime_error("Client: multi-plugin process isolation requires the Windows client package");
        client::Client worker(config, stop_signal, enabled_plugins.front());
        worker.start();
#endif
        LOG_INFO("client agent stopped");
    } catch (const std::exception& e) {
        LOG_ERROR("client failed: " << e.what());
        PauseIfInteractive();
        return 1;
    }
    return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    std::vector<std::string> arguments;
    std::vector<char*> pointers;
    for (int index = 0; index < argc; ++index) {
        const int length = WideCharToMultiByte(CP_UTF8, 0, argv[index], -1, nullptr, 0, nullptr, nullptr);
        std::string value(static_cast<size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, 0, argv[index], -1, value.data(), length, nullptr, nullptr);
        value.resize(value.size() - 1);
        arguments.push_back(std::move(value));
    }
    for (auto& argument : arguments) pointers.push_back(argument.data());
    return RunMain(argc, pointers.data());
}
#else
int main(int argc, char* argv[]) { return RunMain(argc, argv); }
#endif
