#include "client/ProcessAgent.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <condition_variable>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <json.hpp>
#include "common/Logger.h"
#include "common/Config.h"

namespace client {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::wstring Quote(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const auto character : value) {
        if (character == L'\\') { ++slashes; continue; }
        result.append(slashes * (character == L'"' ? 2 : 1), L'\\');
        if (character == L'"') result += L'\\';
        result += character;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

Json ReadJson(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot read " + path.u8string());
    return Json::parse(input);
}

void WriteJson(const std::filesystem::path& path, const Json& value) {
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << value.dump(2) << '\n';
        output.flush();
        if (!output) throw std::runtime_error("cannot write " + path.u8string());
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("cannot publish " + path.u8string());
}

std::vector<std::string> EnabledPlugins(const std::filesystem::path& config) {
    const auto value = ReadJson(config);
    std::vector<std::string> result;
    if (value.contains("plugins") && value["plugins"].is_object()) {
        for (const auto& item : value["plugins"].items()) {
            if (!item.value().is_object() || (item.value().contains("enabled") && !item.value()["enabled"].is_boolean()))
                throw std::runtime_error("Client: plugin configuration requires a boolean enabled: " + item.key());
            if (item.value().value("enabled", false))
                result.push_back(item.key());
        }
    }
    if (result.empty() && value.value("client", Json::object()).value("deployment_endpoint", std::string()).empty())
        throw std::runtime_error("Client: no enabled plugin");
    return result;
}

std::string NewIdentity() {
    std::random_device random;
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (int index = 0; index < 4; ++index) output << std::setw(8) << static_cast<uint32_t>(random());
    return output.str();
}

std::string Hostname() {
    wchar_t hostname[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD count = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(hostname, &count)) return "unknown";
    const int size = WideCharToMultiByte(CP_UTF8, 0, hostname, count, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, hostname, count, result.data(), size, nullptr, nullptr);
    return result;
}

struct HttpHandle {
    HINTERNET value = nullptr;
    explicit HttpHandle(HINTERNET handle) : value(handle) {}
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;
};

bool SendAgentHeartbeat(const std::string& endpoint, const std::string& worker, const Json& body) {
    auto url = std::filesystem::u8path(endpoint + "/api/agents/" + worker + "/heartbeat").wstring();
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS)) return false;
    HttpHandle session(WinHttpOpen(L"TestTracking-Agent/2", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.value) return false;
    WinHttpSetTimeouts(session.value, 1000, 1000, 1000, 1000);
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    HttpHandle connection(WinHttpConnect(session.value, host.c_str(), parts.nPort, 0));
    if (!connection.value) return false;
    HttpHandle request(WinHttpOpenRequest(connection.value, L"POST", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!request.value) return false;
    const auto data = body.dump();
    if (!WinHttpSendRequest(request.value, L"Content-Type: application/json\r\n", static_cast<DWORD>(-1),
                            const_cast<char*>(data.data()), static_cast<DWORD>(data.size()),
                            static_cast<DWORD>(data.size()), 0) || !WinHttpReceiveResponse(request.value, nullptr)) return false;
    DWORD status = 0, size = sizeof(status);
    return WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                               WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) && status == 200;
}

bool UpdateWindowOpen(const std::filesystem::path& config) {
    const auto windows = ReadJson(config).value("client", Json::object()).value("update_windows", Json::array());
    if (windows.empty()) return true;
    const auto now = std::time(nullptr);
    std::tm local{};
    if (localtime_s(&local, &now) != 0) return false;
    const int day = local.tm_wday == 0 ? 7 : local.tm_wday;
    const int previous = day == 1 ? 7 : day - 1;
    const int minute = local.tm_hour * 60 + local.tm_min;
    for (const auto& window : windows) {
        const auto days = window.value("days", Json::array());
        const auto allows = [&](int target) { return days.empty() || std::find(days.begin(), days.end(), target) != days.end(); };
        const auto parse = [](const std::string& value) {
            if (value.size() != 5 || value[2] != ':' ||
                value[0] < '0' || value[0] > '9' || value[1] < '0' || value[1] > '9' ||
                value[3] < '0' || value[3] > '9' || value[4] < '0' || value[4] > '9') return -1;
            const int hour = (value[0] - '0') * 10 + value[1] - '0';
            const int minute = (value[3] - '0') * 10 + value[4] - '0';
            return hour < 24 && minute < 60 ? hour * 60 + minute : -1;
        };
        const int start = parse(window.value("start", std::string()));
        const int end = parse(window.value("end", std::string()));
        if (start < 0 || end < 0) continue;
        if ((start == end && allows(day)) ||
            (start < end && allows(day) && minute >= start && minute < end) ||
            (start > end && ((allows(day) && minute >= start) || (allows(previous) && minute < end)))) return true;
    }
    return false;
}

struct Process {
    HANDLE handle = nullptr;
    DWORD id = 0;
};

class ProcessAgent {
public:
    ProcessAgent(std::filesystem::path executable, std::filesystem::path config,
                 std::shared_ptr<std::atomic<bool>> stop)
        : executable_(std::move(executable)), base_config_(std::filesystem::absolute(config)),
          config_(base_config_), root_(std::filesystem::current_path()), stop_(std::move(stop)) {
        auto base = ReadJson(base_config_);
        auto settings = base.value("client", Json::object());
        worker_id_ = settings.value("worker_id", std::string());
        if (worker_id_.empty() || worker_id_ == "auto") {
            worker_id_ = "client-" + NewIdentity();
            base["client"]["worker_id"] = worker_id_;
            WriteJson(base_config_, base);
            common::Config::instance().load(base_config_.u8string());
            settings = base["client"];
        }
        instance_id_ = NewIdentity();
        hostname_ = Hostname();
        deployment_endpoint_ = settings.value("deployment_endpoint", std::string());
        while (!deployment_endpoint_.empty() && deployment_endpoint_.back() == '/') deployment_endpoint_.pop_back();
        updates_enabled_ = !deployment_endpoint_.empty();
        update_interval_ = std::chrono::seconds(std::clamp(settings.value("deployment_poll_seconds", 30), 1, 3600));
        updater_timeout_ = std::chrono::seconds(std::max(0, settings.value("deployment_stage_timeout_seconds", 0)));
        const auto timeout = std::max(1, settings.value("register_timeout_ms", 5000));
        const auto attempts = std::max(1, settings.value("register_max_attempts", 5));
        startup_timeout_ = std::chrono::milliseconds(std::clamp<int64_t>(
            static_cast<int64_t>(timeout) * attempts + 5000, 5000, 600000));
        state_dir_ = root_ / "deployments";
        if (updates_enabled_) {
            std::filesystem::create_directories(state_dir_);
            if (std::filesystem::exists(state_dir_ / "active.json")) {
                active_ = ReadJson(state_dir_ / "active.json");
                config_ = std::filesystem::u8path(active_.at("config_path").get<std::string>());
                (void)EnabledPlugins(config_);
            }
            if (std::filesystem::exists(state_dir_ / "failed.json")) {
                const auto failure = ReadJson(state_dir_ / "failed.json");
                failed_revision_ = failure.value("revision", std::string());
                if (failed_revision_ != active_.value("revision", std::string())) {
                    pending_failure_ = failure.value("deployment", Json::object());
                    pending_failure_reason_ = failure.value("reason", std::string());
                }
            }
            prepared_revision_ = failed_revision_;
        }
        job_ = CreateJobObjectW(nullptr, nullptr);  // Never inherited by children.
        if (!job_) throw std::runtime_error("cannot create client worker job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            CloseHandle(job_);
            job_ = nullptr;
            throw std::runtime_error("cannot configure client worker job");
        }
    }

    ~ProcessAgent() {
        heartbeat_finished_.store(true);
        heartbeat_wake_.notify_all();
        if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
        StopWorkers();
        if (updater_.handle) {
            TerminateProcess(updater_.handle, 1);
            WaitForSingleObject(updater_.handle, 1000);
            CloseHandle(updater_.handle);
        }
        if (job_) CloseHandle(job_);
    }

    int Run() {
        StartWorkers();
        ReportRestoredFailure();
        RefreshHeartbeat();
        if (updates_enabled_) heartbeat_thread_ = std::thread([this] { HeartbeatLoop(); });
        while (!stop_->load()) {
            PollUpdater();
            Supervise();
            RefreshHeartbeat();
            if (exit_code_ != 0) return exit_code_;
            std::this_thread::sleep_for(20ms);
        }
        return 0;
    }

private:
    enum class Mode { Starting, Running, Draining, Activating, RollingBack };
    struct Worker {
        std::string plugin;
        Process process;
        HANDLE stop = nullptr, drain = nullptr, ready = nullptr;
        HANDLE job = nullptr;
        bool registered = false;
        Clock::time_point started{}, restart_at{};
    };

    void RefreshHeartbeat() {
        if (!updates_enabled_) return;
        const auto now = Clock::now();
        if (now >= next_window_check_) {
            try { update_window_open_ = UpdateWindowOpen(config_); }
            catch (const std::exception& error) {
                update_window_open_ = false;
                LOG_WARN("cannot read agent update window: " << error.what());
            }
            next_window_check_ = now + 1s;
        }
        Json keys = Json::array();
        bool all_ready = true;
        for (const auto& worker : workers_) {
            keys.push_back(worker.plugin);
            all_ready = all_ready && worker.registered && worker.process.handle;
        }
        const std::string status = mode_ == Mode::Draining || mode_ == Mode::Activating ? "updating" :
            (mode_ == Mode::RollingBack ? "error" : (workers_.empty() ? "unconfigured" : (all_ready ? "ready" : "starting")));
        Json heartbeat = {{"instance_id", instance_id_}, {"hostname", hostname_}, {"state", status},
            {"plugin_keys", std::move(keys)}, {"agent_version", "2"},
            {"active_revision", active_.value("revision", std::string())}, {"update_window_open", update_window_open_},
            {"capabilities", Json::array({"deployment_target_id_v1"})}};
        std::lock_guard<std::mutex> lock(heartbeat_mutex_);
        heartbeat_snapshot_ = std::move(heartbeat);
    }

    void HeartbeatLoop() {
        while (!heartbeat_finished_.load()) {
            Json snapshot;
            {
                std::lock_guard<std::mutex> lock(heartbeat_mutex_);
                snapshot = heartbeat_snapshot_;
            }
            try {
                if (!SendAgentHeartbeat(deployment_endpoint_, worker_id_, snapshot))
                    LOG_WARN("agent heartbeat could not reach deployment service");
            } catch (const std::exception& error) {
                LOG_WARN("agent heartbeat failed: " << error.what());
            }
            std::unique_lock<std::mutex> lock(heartbeat_mutex_);
            heartbeat_wake_.wait_for(lock, 3s, [this] { return heartbeat_finished_.load(); });
        }
    }

    Process Launch(const std::filesystem::path& executable, std::wstring command,
                   const std::vector<HANDLE>& signals = {}, bool share_output = false,
                   HANDLE worker_job = nullptr) {
        std::vector<HANDLE> inherited = signals;
        std::vector<HANDLE> temporary;
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        const auto duplicate = [&](HANDLE source, DWORD access, const wchar_t* fallback) {
            HANDLE handle = nullptr;
            if (source && source != INVALID_HANDLE_VALUE)
                DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &handle, 0, TRUE, DUPLICATE_SAME_ACCESS);
            if (!handle) {
                SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
                handle = CreateFileW(fallback, access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            }
            if (!handle || handle == INVALID_HANDLE_VALUE)
                throw std::runtime_error("cannot create worker standard handle");
            inherited.push_back(handle);
            temporary.push_back(handle);
            return handle;
        };
        std::vector<char> attribute_storage;
        bool initialized = false;
        PROCESS_INFORMATION process{};
        try {
            if (share_output) {
                startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
                startup.StartupInfo.hStdInput = duplicate(nullptr, GENERIC_READ, L"NUL");
                startup.StartupInfo.hStdOutput = duplicate(GetStdHandle(STD_OUTPUT_HANDLE), GENERIC_WRITE, L"NUL");
                startup.StartupInfo.hStdError = duplicate(GetStdHandle(STD_ERROR_HANDLE), GENERIC_WRITE, L"NUL");
            }
            DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW;
            if (!inherited.empty()) {
                SIZE_T bytes = 0;
                InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
                attribute_storage.resize(bytes);
                startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
                if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes))
                    throw std::runtime_error("cannot initialize worker handle list");
                initialized = true;
                if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0,
                        PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                        inherited.size() * sizeof(HANDLE), nullptr, nullptr))
                    throw std::runtime_error("cannot restrict worker inherited handles");
                flags |= EXTENDED_STARTUPINFO_PRESENT;
            } else {
                startup.StartupInfo.cb = sizeof(STARTUPINFOW);
            }
            if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
                                !inherited.empty(), flags, nullptr, root_.c_str(),
                                &startup.StartupInfo, &process))
                throw std::runtime_error("cannot start client child process, Windows error " + std::to_string(GetLastError()));
            // A suspended child cannot start an unowned grandchild before job attachment.
            if (!AssignProcessToJobObject(job_, process.hProcess))
                throw std::runtime_error("cannot attach client child to owned job");
            if (worker_job && !AssignProcessToJobObject(worker_job, process.hProcess))
                throw std::runtime_error("cannot attach client worker to its isolated job");
            if (ResumeThread(process.hThread) == static_cast<DWORD>(-1))
                throw std::runtime_error("cannot resume client child");
        } catch (...) {
            if (process.hProcess) { TerminateProcess(process.hProcess, 1); CloseHandle(process.hProcess); }
            if (process.hThread) CloseHandle(process.hThread);
            if (initialized) DeleteProcThreadAttributeList(startup.lpAttributeList);
            for (const auto handle : temporary) CloseHandle(handle);
            throw;
        }
        CloseHandle(process.hThread);
        if (initialized) DeleteProcThreadAttributeList(startup.lpAttributeList);
        for (const auto handle : temporary) CloseHandle(handle);
        return {process.hProcess, process.dwProcessId};
    }

    void StartWorker(Worker& worker) {
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        worker.stop = CreateEventW(&attributes, TRUE, FALSE, nullptr);
        worker.drain = CreateEventW(&attributes, TRUE, FALSE, nullptr);
        worker.ready = CreateEventW(&attributes, TRUE, FALSE, nullptr);
        try {
            if (!worker.stop || !worker.drain || !worker.ready)
                throw std::runtime_error("cannot create client worker events");
            worker.job = CreateJobObjectW(nullptr, nullptr);
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!worker.job || !SetInformationJobObject(worker.job, JobObjectExtendedLimitInformation,
                                                        &limits, sizeof(limits)))
                throw std::runtime_error("cannot create isolated client worker job");
            auto command = Quote(executable_.wstring()) + L" --worker-plugin " +
                Quote(std::filesystem::u8path(worker.plugin).wstring()) + L" --config " + Quote(config_.wstring());
            // One ROUTER identity may own only one registered plugin/session.
            // Retain the historic identity for a single worker and qualify all
            // workers when a physical client runs multiple plugins in parallel.
            if (workers_.size() > 1) command += L" --qualify-worker-id";
            for (const auto& argument : std::vector<std::pair<std::wstring, HANDLE>>{
                    {L" --stop-event ", worker.stop}, {L" --drain-event ", worker.drain}, {L" --ready-event ", worker.ready}})
                command += argument.first + std::to_wstring(reinterpret_cast<uintptr_t>(argument.second));
            worker.process = Launch(executable_, command, {worker.stop, worker.drain, worker.ready}, true, worker.job);
            worker.registered = false;
            worker.started = Clock::now();
            LOG_INFO("client worker started: plugin=" << worker.plugin << " pid=" << worker.process.id);
        } catch (...) {
            CloseWorker(worker);
            throw;
        }
    }

    void StartSupervisedWorker(Worker& worker) {
        try { StartWorker(worker); }
        catch (const std::exception& error) {
            if (!updates_enabled_ || mode_ == Mode::Activating) throw;
            // The independent Agent must remain available for a repair even
            // when the restored worker process cannot be created right now.
            worker.restart_at = Clock::now() + 1s;
            LOG_ERROR("client worker " << worker.plugin << " could not start; will retry: " << error.what());
        }
    }

    void StartWorkers() {
        workers_.clear();
        const auto plugins = EnabledPlugins(config_);
        for (const auto& plugin : plugins) { Worker worker; worker.plugin = plugin; workers_.push_back(worker); }
        LOG_INFO("client agent starting " << workers_.size() << " plugin worker" << (workers_.size() == 1 ? "" : "s"));
        for (auto& worker : workers_) StartSupervisedWorker(worker);
    }

    static void CloseWorker(Worker& worker) {
        // Closing an isolated worker job also kills a business executable left
        // behind by a crashed plugin, without interrupting the sibling workers.
        if (worker.job) CloseHandle(worker.job);
        if (worker.process.handle) CloseHandle(worker.process.handle);
        if (worker.stop) CloseHandle(worker.stop);
        if (worker.drain) CloseHandle(worker.drain);
        if (worker.ready) CloseHandle(worker.ready);
        worker.process = {};
        worker.stop = worker.drain = worker.ready = nullptr;
        worker.job = nullptr;
    }

    void StopWorkers() {
        for (auto& worker : workers_) if (worker.stop) SetEvent(worker.stop);
        const auto deadline = Clock::now() + 1500ms;
        for (auto& worker : workers_) {
            if (worker.process.handle) {
                const auto remaining = std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
                if (WaitForSingleObject(worker.process.handle, static_cast<DWORD>(remaining)) == WAIT_TIMEOUT) {
                    TerminateProcess(worker.process.handle, 1);
                    WaitForSingleObject(worker.process.handle, 1000);
                }
            }
            CloseWorker(worker);
        }
        workers_.clear();
    }

    void Report(const Json& deployment, const char* state, const std::string& message = {}) {
        if (deployment.value("revision", std::string()).empty()) return;
        Json components = Json::array();
        for (const auto& component : deployment.value("components", Json::array())) {
            components.push_back({{"slot", component.at("slot")}, {"artifact_id", component.at("artifact_id")},
                {"state", std::string(state) == "ready" ? "active" : std::string(state) == "failed" ? "failed" : component.value("state", "staged")}});
        }
        WriteJson(state_dir_ / "agent_report.json", {{"desired_revision", deployment.at("revision")},
            {"reservation_id", deployment.value("reservation_id", std::string())},
            {"state", state}, {"components", components}, {"agent_version", "2"}, {"message", message}});
        next_update_ = Clock::now();
    }

    void ReportRestoredFailure() {
        if (pending_failure_.empty()) return;
        // Call only after the replacement workers have stopped and config_ is
        // restored to the committed/base generation. Its workers need not be
        // registered: a scheduler already requiring the new program version
        // may reject the old one until the user publishes a corrected release.
        Report(pending_failure_, "failed", pending_failure_reason_);
        WriteJson(state_dir_ / "failed.json", {{"revision", failed_revision_}, {"reason", pending_failure_reason_}});
        pending_failure_ = Json::object();
        pending_failure_reason_.clear();
    }

    void ActivationFailed(const std::string& reason) {
        LOG_ERROR("deployment activation failed; restoring previous configuration: " << reason);
        StopWorkers();
        failed_revision_ = staged_.value("revision", std::string());
        pending_failure_ = staged_;
        pending_failure_reason_ = reason;
        WriteJson(state_dir_ / "failed.json", {{"revision", failed_revision_}, {"reason", reason}, {"deployment", staged_}});
        staged_ = Json();
        config_ = active_.contains("config_path") ? std::filesystem::u8path(active_.at("config_path").get<std::string>()) : base_config_;
        mode_ = Mode::RollingBack;
        StartWorkers();
        ReportRestoredFailure();
    }

    void Supervise() {
        const auto now = Clock::now();
        bool all_ready = true, all_exited = true;
        for (auto& worker : workers_) {
            if (!worker.process.handle) {
                all_ready = false;
                if (mode_ != Mode::Draining && now >= worker.restart_at) StartSupervisedWorker(worker);
                if (worker.process.handle) all_exited = false;
                continue;
            }
            if (WaitForSingleObject(worker.ready, 0) == WAIT_OBJECT_0) worker.registered = true;
            const bool exited = WaitForSingleObject(worker.process.handle, 0) != WAIT_TIMEOUT;
            const bool timed_out = !worker.registered && now - worker.started > startup_timeout_;
            if (exited || timed_out) {
                DWORD code = 1;
                if (exited) GetExitCodeProcess(worker.process.handle, &code);
                else { TerminateProcess(worker.process.handle, 1); WaitForSingleObject(worker.process.handle, 1000); }
                const bool was_ready = worker.registered;
                CloseWorker(worker);
                all_ready = false;
                if (mode_ == Mode::Draining) continue;
                if (mode_ == Mode::Activating) { ActivationFailed("worker " + worker.plugin + " failed registration/startup"); return; }
                if (!was_ready && mode_ == Mode::Starting && !updates_enabled_) {
                    LOG_ERROR("client worker " << worker.plugin << " exited during startup, code=" << code);
                    exit_code_ = code == 0 ? 1 : static_cast<int>(code);
                    return;
                }
                LOG_WARN("client worker " << worker.plugin << " exited; restarting");
                worker.restart_at = now + 1s;
            } else {
                all_exited = false;
                all_ready = all_ready && worker.registered;
            }
        }
        if (mode_ == Mode::Draining && all_exited) {
            StopWorkers();
            if (!UpdateWindowOpen(config_)) {
                // A long task may finish after the download window has closed.
                // Resume the old release and retry activation during the next window.
                Report(staged_, "waiting_window", "Current tasks finished after the update window closed");
                staged_ = Json();
                mode_ = Mode::Running;
                StartWorkers();
                return;
            }
            config_ = std::filesystem::u8path(staged_.at("config_path").get<std::string>());
            Report(staged_, "applying");
            mode_ = Mode::Activating;
            try { StartWorkers(); }
            catch (const std::exception& error) { ActivationFailed(error.what()); }
            return;
        }
        if (all_ready && mode_ != Mode::Running && mode_ != Mode::Draining) {
            if (mode_ == Mode::Activating) {
                try {
                    WriteJson(state_dir_ / "active.json", staged_);
                } catch (const std::exception& error) { ActivationFailed(error.what()); return; }
                active_ = staged_;
                staged_ = Json();
                failed_revision_.clear();
                std::error_code ignored;
                std::filesystem::remove(state_dir_ / "failed.json", ignored);
                Report(active_, "ready");
                LOG_INFO("deployment activated: " << active_.at("revision"));
            } else if (mode_ == Mode::Starting && active_.contains("revision")) {
                bool pending_failure = false;
                const auto report_file = state_dir_ / "agent_report.json";
                if (std::filesystem::exists(report_file)) {
                    const auto report = ReadJson(report_file);
                    pending_failure = report.value("desired_revision", std::string()) == failed_revision_ &&
                        failed_revision_ != active_.value("revision", std::string());
                }
                // A rollback report may still await HTTP delivery after an Agent
                // restart. The restored older release must not overwrite it.
                if (!pending_failure) Report(active_, "ready");
            }
            mode_ = Mode::Running;
        }
    }

    void PollUpdater() {
        if (!updates_enabled_) return;
        const auto now = Clock::now();
        if (updater_.handle) {
            if (WaitForSingleObject(updater_.handle, 0) == WAIT_TIMEOUT) {
                if (updater_timeout_.count() > 0 && now - updater_started_ > updater_timeout_) {
                    LOG_ERROR("client updater exceeded configured deployment_stage_timeout_seconds");
                    TerminateProcess(updater_.handle, 1);
                }
                return;
            }
            DWORD code = 1;
            GetExitCodeProcess(updater_.handle, &code);
            CloseHandle(updater_.handle);
            updater_ = {};
            next_update_ = now + update_interval_;
            try {
                const auto result = ReadJson(state_dir_ / "update_result.json");
                const auto revision = result.contains("revision") && result["revision"].is_string()
                    ? result["revision"].get<std::string>() : std::string();
                const auto status = result.value("status", std::string());
                if (!revision.empty() && revision == active_.value("revision", std::string())) {
                    // A helper can start before activation and return after the
                    // workers register. Its preparation snapshot cannot downgrade
                    // the committed generation, or overwrite another desired
                    // generation's pending failure report with an old ready state.
                    if (status == "failed")
                        LOG_WARN("ignoring preparation failure for active deployment " << revision
                                 << ": " << result.value("message", std::string()));
                } else if (status == "rejected") {
                    // The service already has this terminal failure. Leave its
                    // durable report intact and wait for a newly reserved revision.
                    next_update_ = now + update_interval_;
                } else if (!revision.empty() && revision == failed_revision_) {
                    // failed.json may survive a crash before its failure report.
                    // The updater has confirmed this is still the desired
                    // generation and supplies its complete component list.
                    const auto failure = ReadJson(state_dir_ / "failed.json");
                    if (pending_failure_.empty() && mode_ != Mode::Draining && mode_ != Mode::Activating)
                        Report(result, "failed", failure.value("reason", std::string("deployment activation failed")));
                    next_update_ = now + update_interval_;
                } else if (status == "waiting_window" && !revision.empty()) {
                    Report(result, "waiting_window");
                    next_update_ = now + update_interval_;
                } else if (status == "failed") {
                    if (!revision.empty()) {
                        failed_revision_ = revision;
                        const auto reason = result.value("message", std::string("staging failed"));
                        WriteJson(state_dir_ / "failed.json", {{"revision", revision}, {"reason", reason}});
                        Report(result, "failed", reason);
                    }
                    LOG_WARN("client deployment staging failed: " << result.value("message", std::string()));
                    next_update_ = now + update_interval_;
                } else if (code == 0 && status == "staged" && !revision.empty() &&
                           revision != active_.value("revision", std::string()) && revision != failed_revision_ &&
                           (mode_ == Mode::Running || mode_ == Mode::Starting || mode_ == Mode::RollingBack)) {
                    // Validate before draining the current healthy workers.
                    try {
                        (void)EnabledPlugins(std::filesystem::u8path(result.at("config_path").get<std::string>()));
                    } catch (const std::exception& error) {
                        failed_revision_ = revision;
                        WriteJson(state_dir_ / "failed.json", {{"revision", revision}, {"reason", error.what()}});
                        Report(result, "failed", error.what());
                        return;
                    }
                    staged_ = result;
                    prepared_revision_ = revision;
                    Report(staged_, "waiting_idle", "Waiting for running tasks and result acknowledgements");
                    mode_ = Mode::Draining;
                    for (auto& worker : workers_) if (worker.drain) SetEvent(worker.drain);
                    LOG_INFO("deployment staged; draining current tasks and reports");
                }
            } catch (const std::exception& error) {
                LOG_WARN("client updater result unavailable: " << error.what());
            }
        }
        if (now < next_update_ || updater_.handle) return;
        const auto script = executable_.parent_path() / "tools" / "client_update.ps1";
        wchar_t system_directory[MAX_PATH]{};
        GetSystemDirectoryW(system_directory, MAX_PATH);
        const auto powershell = std::filesystem::path(system_directory) / "WindowsPowerShell" / "v1.0" / "powershell.exe";
        try {
            std::error_code ignored;
            std::filesystem::remove(state_dir_ / "update_result.json", ignored);
            const auto command = Quote(powershell.wstring()) + L" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " +
                Quote(script.wstring()) + L" -ClientRoot " + Quote(root_.wstring()) + L" -ConfigPath " +
                Quote(base_config_.wstring()) + L" -ResultPath " + Quote((state_dir_ / "update_result.json").wstring()) +
                L" -ReportPath " + Quote((state_dir_ / "agent_report.json").wstring()) +
                L" -EffectiveConfigPath " + Quote(config_.wstring()) +
                L" -CurrentRevision " + Quote(std::filesystem::u8path(active_.value("revision", std::string())).wstring()) +
                L" -PreparedRevision " + Quote(std::filesystem::u8path(prepared_revision_).wstring());
            updater_ = Launch(powershell, command);
            updater_started_ = now;
        } catch (const std::exception& error) {
            LOG_WARN("cannot launch client updater: " << error.what());
            next_update_ = now + update_interval_;
        }
    }

    std::filesystem::path executable_, base_config_, config_, root_, state_dir_;
    std::shared_ptr<std::atomic<bool>> stop_;
    HANDLE job_ = nullptr;
    std::vector<Worker> workers_;
    Process updater_;
    Json active_ = Json::object(), staged_, pending_failure_ = Json::object();
    std::string failed_revision_, prepared_revision_, pending_failure_reason_;
    std::string deployment_endpoint_, worker_id_, instance_id_, hostname_;
    std::mutex heartbeat_mutex_;
    std::condition_variable heartbeat_wake_;
    std::thread heartbeat_thread_;
    std::atomic<bool> heartbeat_finished_{false};
    Json heartbeat_snapshot_ = Json::object();
    bool update_window_open_ = true;
    Clock::time_point next_window_check_{};
    Mode mode_ = Mode::Starting;
    bool updates_enabled_ = false;
    int exit_code_ = 0;
    std::chrono::seconds update_interval_{30};
    std::chrono::seconds updater_timeout_{0};
    std::chrono::milliseconds startup_timeout_{30000};
    Clock::time_point next_update_{}, updater_started_{};
};
}

int RunProcessAgent(const std::filesystem::path& executable, const std::filesystem::path& config,
                    const std::shared_ptr<std::atomic<bool>>& stop) {
    ProcessAgent agent(executable, config, stop);
    return agent.Run();
}
}
#endif
