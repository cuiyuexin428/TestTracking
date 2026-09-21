#include <string>
#include <thread>
#include <chrono>
#include <json.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

#include "common/WorkerPlugin.h"

namespace {

class RetryProbe : public common::WorkerPlugin {
   public:
    std::string pluginType() const override { return "client-retry-probe"; }
    bool init(const std::string& config) override {
        const auto value = nlohmann::json::parse(config);
        process_probe_ = value.value("process_probe", false);
        task_sleep_ms_ = value.value("task_sleep_ms", 0);
#ifdef _WIN32
        if (process_probe_) {
            wchar_t directory[MAX_PATH]{};
            GetSystemDirectoryW(directory, MAX_PATH);
            std::wstring executable = std::wstring(directory) + L"\\cmd.exe";
            std::wstring command = L"\"" + executable + L"\" /c ping.exe -n 120 -w 1000 127.0.0.1 > nul";
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                                CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return false;
            child_pid_ = process.dwProcessId;
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
#endif
        return true;
    }

    bool collectRegistrationInfo(std::string* payload) override {
        *payload = process_probe_ ? std::to_string(child_pid_) : "registration-" + std::to_string(++registrations_);
        return true;
    }

    common::RegisterOutcome handleValidationReply(const std::string&) override {
        return process_probe_ ? common::RegisterOutcome::kReady : common::RegisterOutcome::kRetry;
    }

    bool execute(const common::Task&, common::TaskResult&) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(task_sleep_ms_));
        return process_probe_;
    }

   private:
    int registrations_ = 0;
    bool process_probe_ = false;
    unsigned long child_pid_ = 0;
    int task_sleep_ms_ = 0;
};

}  // namespace

#ifdef _WIN32
#define TEST_EXPORT __declspec(dllexport)
#else
#define TEST_EXPORT __attribute__((visibility("default")))
#endif

extern "C" TEST_EXPORT common::WorkerPlugin* CreateClientPlugin() {
    return new RetryProbe;
}

extern "C" TEST_EXPORT void DestroyClientPlugin(common::WorkerPlugin* plugin) {
    delete plugin;
}
