#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

#include "client/Client.h"
#include "common/Config.h"
#include "common/Protocol.h"
#include "common/zmq_helper.h"

namespace {
using namespace std::chrono_literals;
void Require(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}

class Peer {
   public:
    explicit Peer(std::string type = "demo") : plugin_type(std::move(type)) {
        for (int port = 28700; port < 28800; ++port) {
            try {
                endpoint = "tcp://127.0.0.1:" + std::to_string(port);
                auto candidate = std::make_unique<common::ZmqRouter>(endpoint);
                candidate->bind();
                socket = std::move(candidate);
                return;
            } catch (const std::exception&) {
            }
        }
        throw std::runtime_error("no available loopback test port");
    }

    void Send(common::Envelope envelope) {
        envelope.set_plugin_type(plugin_type);
        if (envelope.session_id().empty()) envelope.set_session_id(session);
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (!socket->trySendMsgParts({identity, envelope.SerializeAsString()})) {
            if (std::chrono::steady_clock::now() > deadline)
                throw std::runtime_error("peer send blocked");
            std::this_thread::sleep_for(1ms);
        }
    }

    common::Envelope Receive(std::function<bool(const common::Envelope&)> match,
                             std::chrono::milliseconds timeout = 3s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!socket->waitReadable(10ms)) continue;
            const auto frames = socket->recvMsgParts();
            Require(frames.size() == 2, "unexpected ROUTER frame count");
            identity = frames[0];
            common::Envelope envelope;
            Require(envelope.ParseFromString(frames[1]), "invalid protocol payload");
            if (envelope.type() == common::HEARTBEAT) {
                ++heartbeats;
                common::Envelope ack;
                ack.set_type(common::HEARTBEAT_ACK);
                ack.set_session_id(envelope.session_id());
                ack.mutable_heartbeat_ack()->set_registered(session == envelope.session_id());
                Send(ack);
            }
            if (envelope.type() == common::REQUEST_TASK) ++requests;
            if (envelope.type() == common::TASK_REPORT) {
                ++reports;
                last_report = envelope.task_report().result();
            }
            if (match(envelope)) return envelope;
            if (envelope.type() == common::REQUEST_TASK) {
                common::Envelope idle;
                idle.set_type(common::NO_TASK);
                idle.mutable_no_task();
                Send(idle);
            }
        }
        return {};
    }

    common::Envelope Wait(common::MessageType type, std::chrono::milliseconds timeout = 3s) {
        auto envelope =
            Receive([&](const common::Envelope& e) { return e.type() == type; }, timeout);
        Require(envelope.type() == type, "timed out waiting for expected protocol message");
        return envelope;
    }

    void Pump(std::chrono::milliseconds duration) {
        Receive([](const common::Envelope&) { return false; }, duration);
    }

    void Register(const common::Envelope& request, const std::string& new_session) {
        session = new_session;
        common::Envelope response;
        response.set_type(common::REGISTER_RESULT);
        response.mutable_register_result()->set_accepted(true);
        response.mutable_register_result()->set_plugin_found(true);
        response.mutable_register_result()->set_registration_id(
            request.register_().registration_id());
        response.mutable_register_result()->set_protocol_version(common::kProtocolVersion);
        response.mutable_register_result()->add_capabilities(common::kAttemptIdsCapability);
        Send(response);
    }

    void RegisterLegacy(const common::Envelope& request, const std::string& new_session) {
        session = new_session;
        common::Envelope response;
        response.set_type(common::REGISTER_RESULT);
        response.mutable_register_result()->set_accepted(true);
        response.mutable_register_result()->set_plugin_found(true);
        response.mutable_register_result()->set_registration_id(
            request.register_().registration_id());
        Send(response);
    }

    void Task(const std::string& run, const std::string& attempt) {
        common::Envelope envelope;
        envelope.set_type(common::TASK);
        envelope.mutable_task()->set_run_id(run);
        auto* task = envelope.mutable_task()->add_tasks();
        task->set_task_id("same-task-id");
        task->set_run_id(run);
        task->set_attempt_id(attempt);
        // Empty is a valid default DemoTask payload.
        Send(envelope);
    }

    void Ack(const std::string& attempt, bool accepted, bool retryable) {
        common::Envelope envelope;
        envelope.set_type(common::TASK_REPORT_ACK);
        auto* ack = envelope.mutable_task_report_ack();
        ack->set_task_id("same-task-id");
        ack->set_attempt_id(attempt);
        ack->set_accepted(accepted);
        ack->set_retryable(retryable);
        Send(envelope);
    }

    std::string endpoint, identity, session, plugin_type;
    int heartbeats = 0, requests = 0, reports = 0;
    common::TaskResult last_report;

   private:
    std::unique_ptr<common::ZmqRouter> socket;
};

void TestClient(const std::filesystem::path& binary_directory) {
    Peer peer;
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("testtracking-protocol-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    const auto config_file = directory / "client.json";
#ifdef _WIN32
    const auto plugin_file = binary_directory / "demo.dll";
#else
    const auto plugin_file = binary_directory / "libdemo.so";
#endif
    nlohmann::json json = {
        {"scheduler", {{"endpoint", peer.endpoint}, {"poll_timeout_ms", 5}}},
        {"client",
         {{"worker_id", "protocol-client"},
          {"heartbeat_interval_ms", 40},
          {"request_timeout_ms", 500},
          {"register_timeout_ms", 300},
          {"register_max_attempts", 10},
          {"report_retry_ms", 60},
          {"request_backoff_initial_ms", 10},
          {"request_backoff_max_ms", 20}}},
        {"plugins",
         {{"demo", {{"dll", plugin_file.string()}, {"enabled", true}, {"task_sleep_ms", 700}}}}}};
    std::ofstream(config_file) << json.dump();
    auto& config = common::Config::instance();
    config.load(config_file.string());
    auto stop = std::make_shared<std::atomic<bool>>(false);
    auto running = std::async(std::launch::async, [&] {
        client::Client client(config, stop);
        client.start();
    });
    try {
        const auto first_registration = peer.Wait(common::REGISTER);
        Require(first_registration.register_().protocol_version() == common::kProtocolVersion &&
                    first_registration.register_().capabilities_size() == 1 &&
                    first_registration.register_().capabilities(0) == common::kAttemptIdsCapability,
                "client omitted the framework compatibility handshake");
        // A delayed reply for another nonce cannot start a session.
        auto stale = first_registration;
        stale.mutable_register_()->set_registration_id("stale-nonce");
        peer.Register(stale, "stale-session");
        peer.Pump(70ms);
        Require(peer.requests == 0, "stale registration reply was accepted");
        peer.Register(first_registration, "session-one");
        peer.Wait(common::REQUEST_TASK);
        peer.Task("run-one", "attempt-one");
        peer.Wait(common::TASK_ACK);
        peer.Task("run-one", "attempt-one");  // Receipt retry must not execute twice.
        peer.Wait(common::TASK_ACK);
        peer.Wait(common::TASK_REPORT);
        Require(peer.last_report.run_id() == "run-one" &&
                    peer.last_report.attempt_id() == "attempt-one",
                "report lost attempt/run identity");
        const int requests_before_ack = peer.requests;
        const int reports_before_ack = peer.reports;
        peer.Ack("obsolete-attempt", true, false);
        peer.Pump(230ms);
        Require(peer.reports >= reports_before_ack + 2, "unacknowledged result was not retried");
        Require(peer.requests == requests_before_ack, "wrong attempt ACK freed a task slot");
        peer.Ack("attempt-one", false, true);
        peer.Wait(common::TASK_REPORT);
        peer.Ack("attempt-one", true, false);
        peer.Wait(common::REQUEST_TASK);

        peer.Task("run-two", "attempt-two");
        peer.Wait(common::TASK_ACK);
        common::Envelope pause;
        pause.set_type(common::STOP);
        pause.mutable_stop()->set_run_id("run-two");
        peer.Send(pause);
        const int heartbeat_count = peer.heartbeats;
        const int reports_before_stop = peer.reports;
        peer.Pump(300ms);
        Require(peer.heartbeats >= heartbeat_count + 4, "STOP blocked heartbeat transmission");
        Require(peer.reports == reports_before_stop, "stopped execution published a report");

        peer.session.clear();  // Model a scheduler restart forgetting the old session.
        const auto new_registration = peer.Wait(common::REGISTER);
        Require(new_registration.register_().registration_id() !=
                    first_registration.register_().registration_id(),
                "new session reused an old registration nonce");
        peer.Register(new_registration, "session-two");
        const int paused_requests = peer.requests;
        peer.Pump(180ms);
        Require(peer.requests == paused_requests,
                "re-registration unexpectedly cleared paused state");
        common::Envelope wake;
        wake.set_type(common::WAKE);
        wake.mutable_wake();
        peer.Send(wake);
        peer.Wait(common::REQUEST_TASK);
        peer.Task("run-three", "attempt-three");
        peer.Wait(common::TASK_ACK);
        peer.Ack("attempt-one", true, false);
        peer.Wait(common::TASK_REPORT);
        Require(peer.last_report.attempt_id() == "attempt-three" &&
                    peer.last_report.run_id() == "run-three",
                "new session was contaminated by old completion");
        peer.Ack("attempt-three", false, false);
        peer.Wait(common::REQUEST_TASK);
        peer.session.clear();
        const auto incompatible_registration = peer.Wait(common::REGISTER);
        peer.RegisterLegacy(incompatible_registration, "legacy-session");
        Require(running.wait_for(2s) == std::future_status::ready,
                "client accepted an incompatible scheduler protocol");
        bool explicit_rejection = false;
        try {
            running.get();
        } catch (const std::runtime_error& error) {
            explicit_rejection =
                std::string(error.what()).find("incompatible scheduler protocol") !=
                std::string::npos;
        }
        Require(explicit_rejection, "client did not report the protocol incompatibility");
        stop->store(true);
    } catch (...) {
        stop->store(true);
        if (running.valid()) running.wait();
        throw;
    }
}

void TestInitialRegistrationRetryLimit(const std::filesystem::path& binary_directory) {
    Peer peer("client-retry-probe");
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("testtracking-register-retry-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
#ifdef _WIN32
    const auto plugin_file = binary_directory / "client_retry_probe.dll";
#else
    const auto plugin_file = binary_directory / "libclient_retry_probe.so";
#endif
    const auto config_file = directory / "client.json";
    const nlohmann::json json = {
        {"scheduler", {{"endpoint", peer.endpoint}, {"poll_timeout_ms", 5}}},
        {"client",
         {{"worker_id", "registration-retry-client"},
          {"register_timeout_ms", 200},
          {"register_max_attempts", 3}}},
        {"plugins", {{"retry-probe", {{"dll", plugin_file.string()}, {"enabled", true}}}}}};
    std::ofstream(config_file) << json.dump();
    auto& config = common::Config::instance();
    config.load(config_file.string());
    auto stop = std::make_shared<std::atomic<bool>>(false);
    auto running = std::async(std::launch::async, [&] {
        client::Client client(config, stop);
        client.start();
    });

    try {
        std::string previous_registration_id;
        for (int attempt = 1; attempt <= 3; ++attempt) {
            const auto registration = peer.Wait(common::REGISTER);
            Require(registration.register_().payload() == "registration-" + std::to_string(attempt),
                    "registration retry did not refresh plugin metadata");
            Require(registration.register_().registration_id() != previous_registration_id,
                    "registration retry reused its request id");
            previous_registration_id = registration.register_().registration_id();
            peer.Register(registration, "retry-session");
        }
        Require(running.wait_for(1s) == std::future_status::ready,
                "plugin registration retries bypassed register_max_attempts");
        bool attempt_limit_reported = false;
        try {
            running.get();
        } catch (const std::runtime_error& error) {
            attempt_limit_reported =
                std::string(error.what()).find("registration failed after configured attempts") !=
                std::string::npos;
        }
        Require(attempt_limit_reported, "registration retry limit did not fail explicitly");
        stop->store(true);
    } catch (...) {
        stop->store(true);
        if (running.valid()) running.wait();
        throw;
    }
}

void TestDeploymentDrain(const std::filesystem::path& binary_directory) {
    Peer peer;
    const auto config_file = std::filesystem::temp_directory_path() /
        ("drain-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
#ifdef _WIN32
    const auto plugin = binary_directory / "demo.dll";
#else
    const auto plugin = binary_directory / "libdemo.so";
#endif
    std::ofstream(config_file) << nlohmann::json({
        {"scheduler", {{"endpoint", peer.endpoint}, {"poll_timeout_ms", 5}}},
        {"client", {{"heartbeat_interval_ms", 40}, {"request_timeout_ms", 500}, {"report_retry_ms", 60}}},
        {"plugins", {{"demo", {{"enabled", true}, {"dll", plugin.u8string()}, {"task_sleep_ms", 300}}}}}
    }).dump();
    auto& config = common::Config::instance();
    config.load(config_file.u8string());
    auto stop = std::make_shared<std::atomic<bool>>(false);
    auto drain = std::make_shared<std::atomic<bool>>(false);
    std::atomic<bool> ready{false};
    auto running = std::async(std::launch::async, [&] {
        client::Client client(config, stop, {}, false, drain, [&] { ready.store(true); });
        client.start();
    });
    try {
        const auto registration = peer.Wait(common::REGISTER);
        Require(!ready.load(), "worker signaled ready before registration was accepted");
        peer.Register(registration, "drain-session");
        peer.Wait(common::REQUEST_TASK);
        Require(ready.load(), "worker did not signal registration readiness");
        peer.Task("drain-run", "active");
        peer.Wait(common::TASK_ACK);
        drain->store(true);
        peer.Task("drain-run", "late");
        peer.Wait(common::TASK_REJECT);
        peer.Wait(common::TASK_REPORT);
        Require(peer.last_report.attempt_id() == "active", "draining cancelled active execution");
        const auto heartbeats = peer.heartbeats;
        const auto requests = peer.requests;
        peer.Pump(200ms);
        Require(peer.heartbeats > heartbeats, "drain stopped heartbeats before report acknowledgement");
        Require(peer.requests == requests, "drain requested additional tasks");
        Require(running.wait_for(0ms) != std::future_status::ready, "drain discarded unacknowledged reports");
        peer.Ack("active", true, false);
        Require(running.wait_for(1s) == std::future_status::ready, "drain did not finish after report acknowledgement");
        running.get();
    } catch (...) { stop->store(true); if (running.valid()) running.wait(); throw; }
}

#ifdef _WIN32
class AgentProcess {
public:
    AgentProcess(const std::filesystem::path& directory, const std::filesystem::path& config) {
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE input_read = nullptr;
        Require(CreatePipe(&input_read, &input_, &attributes, 0), "cannot create test agent input pipe");
        SetHandleInformation(input_, HANDLE_FLAG_INHERIT, 0);
        output_ = CreateFileW((directory / L"agent.log").c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, CREATE_ALWAYS, 0, nullptr);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = input_read;
        startup.hStdOutput = output_;
        startup.hStdError = output_;
        const auto executable = directory / "client.exe";
        auto command = L"\"" + executable.wstring() + L"\" \"" + config.wstring() + L"\"";
        const bool started = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process_);
        CloseHandle(input_read);
        Require(started, "cannot start test agent");
        CloseHandle(process_.hThread);
    }
    ~AgentProcess() {
        if (process_.hProcess) { TerminateProcess(process_.hProcess, 9); WaitForSingleObject(process_.hProcess, 2000); CloseHandle(process_.hProcess); }
        if (input_) CloseHandle(input_);
        if (output_ && output_ != INVALID_HANDLE_VALUE) CloseHandle(output_);
    }
    void Kill() { Require(TerminateProcess(process_.hProcess, 9), "cannot kill test agent"); WaitForSingleObject(process_.hProcess, 2000); }
    void Quit() {
        DWORD written = 0;
        Require(WriteFile(input_, "quit\n", 5, &written, nullptr), "cannot request agent quit");
        Require(WaitForSingleObject(process_.hProcess, 3000) == WAIT_OBJECT_0, "agent quit did not finish");
        DWORD code = 1;
        GetExitCodeProcess(process_.hProcess, &code);
        Require(code == 0, "agent quit returned a failure");
    }
    std::vector<HANDLE> Workers() const {
        std::vector<HANDLE> handles;
        const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (entry.th32ParentProcessID == process_.dwProcessId && std::wstring(entry.szExeFile) == L"client.exe") {
                    auto handle = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
                    if (handle) handles.push_back(handle);
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return handles;
    }
private:
    PROCESS_INFORMATION process_{};
    HANDLE input_ = nullptr, output_ = nullptr;
};

void CopyAgentPackage(const std::filesystem::path& source, const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory / "tools");
    for (const auto& file : std::filesystem::directory_iterator(source)) {
        if (file.path().extension() == L".dll" || file.path().filename() == L"client.exe") {
            std::error_code error;
            std::filesystem::create_hard_link(file.path(), directory / file.path().filename(), error);
            if (error) std::filesystem::copy_file(file.path(), directory / file.path().filename());
        }
    }
    // The staging script is tested separately against a real HTTP artifact server.
    // This fixture controls when a prepared generation reaches the Agent.
    std::ofstream(directory / "tools/client_update.ps1") << R"PS(
param($ClientRoot, $ConfigPath, $ResultPath, $ReportPath, $EffectiveConfigPath, $CurrentRevision = '', $PreparedRevision = '')
$ErrorActionPreference = 'Stop'
$inbox = Join-Path $ClientRoot 'inbox.json'
$value = if (Test-Path -LiteralPath $inbox) { [IO.File]::ReadAllText($inbox) } else { '{"status":"current"}' }
$result = $value | ConvertFrom-Json
if ($result.test_late_status) {
    $marker = Join-Path $ClientRoot $result.test_token
    if ($CurrentRevision -ceq $result.revision) {
        $result.status = 'current'
        [IO.File]::WriteAllText(($marker + '.current.json'), $value)
    } elseif ($PreparedRevision -ceq $result.revision) {
        $captured = @{ current = $CurrentRevision; prepared = $PreparedRevision } | ConvertTo-Json
        [IO.File]::WriteAllText(($marker + '.started.json'), $captured)
        $deadline = [DateTime]::UtcNow.AddSeconds(20)
        while (-not [IO.File]::Exists($marker + '.release')) {
            if ([DateTime]::UtcNow -ge $deadline) { throw 'Timed out waiting for late updater result release' }
            Start-Sleep -Milliseconds 10
        }
        $result.status = $result.test_late_status
    }
    $value = $result | ConvertTo-Json -Depth 20
}
[IO.File]::WriteAllText($ResultPath, $value, (New-Object Text.UTF8Encoding($false)))
if ($result.status -eq 'failed') { exit 1 }
)PS";
}

void TestAgentProcesses(const std::filesystem::path& binaries) {
    Peer peer("client-retry-probe");
    const auto directory = std::filesystem::temp_directory_path() /
        (L"agent-processes-测试-" + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
    CopyAgentPackage(binaries, directory);
    const auto path = directory / L"配置.json";
    const nlohmann::json config = {
        {"scheduler", {{"endpoint", peer.endpoint}, {"poll_timeout_ms", 5}}},
        {"client", {{"worker_id", "agent"}, {"heartbeat_interval_ms", 40}, {"request_timeout_ms", 500}}},
        {"plugins", {{"demo", {{"enabled", true}, {"dll", "client_retry_probe.dll"}, {"process_probe", true}}}}}};
    std::ofstream(path) << config.dump();
    AgentProcess agent(directory, path.filename());
    std::vector<HANDLE> business_processes;
    auto registration = peer.Wait(common::REGISTER);
    const auto child = OpenProcess(SYNCHRONIZE, FALSE, std::stoul(registration.register_().payload()));
    Require(child, "worker did not start its business subprocess");
    business_processes.push_back(child);
    peer.Register(registration, "agent-test-session");
    Require(peer.identity == "agent", "single-plugin worker changed its identity");
    peer.Pump(100ms);
    auto workers = agent.Workers();
    Require(workers.size() == 1, "Agent must start exactly one isolated worker");
    Require(TerminateProcess(workers.front(), 8), "cannot crash test worker");
    Require(WaitForSingleObject(workers.front(), 2000) == WAIT_OBJECT_0, "test worker did not exit");
    registration = peer.Wait(common::REGISTER, 4s);
    Require(peer.identity == "agent", "restarted worker lost its identity");
    peer.Register(registration, "agent-test-session");
    Require(WaitForSingleObject(business_processes.front(), 0) == WAIT_OBJECT_0,
            "worker crash left its business process tree running");
    const auto replacement_child = OpenProcess(SYNCHRONIZE, FALSE, std::stoul(registration.register_().payload()));
    Require(replacement_child, "restarted worker did not start its business subprocess");
    business_processes.push_back(replacement_child);
    peer.Pump(100ms);
    for (const auto handle : workers) CloseHandle(handle);
    workers = agent.Workers();
    Require(workers.size() == 1, "worker crash was not recovered");
    agent.Kill();
    for (const auto handle : workers) {
        Require(WaitForSingleObject(handle, 2000) == WAIT_OBJECT_0, "killing Agent left a worker alive (job handle leaked)");
        CloseHandle(handle);
    }
    for (const auto handle : business_processes) {
        Require(WaitForSingleObject(handle, 2000) == WAIT_OBJECT_0, "killing Agent left a business subprocess alive");
        CloseHandle(handle);
    }
}

void TestAgentMultiplePlugins(const std::filesystem::path& binaries) {
    // Model independent scheduler sessions on one ROUTER, including identical
    // business task IDs. No packet from one worker may complete its sibling.
    const auto directory = std::filesystem::temp_directory_path() /
        (L"agent-multiplugin-" + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
    CopyAgentPackage(binaries, directory);
    std::unique_ptr<common::ZmqRouter> socket;
    std::string endpoint;
    for (int port = 28800; port < 28900; ++port) {
        try {
            endpoint = "tcp://127.0.0.1:" + std::to_string(port);
            auto candidate = std::make_unique<common::ZmqRouter>(endpoint);
            candidate->bind(); socket = std::move(candidate); break;
        } catch (const std::exception&) {}
    }
    Require(socket != nullptr, "cannot bind multi-plugin test endpoint");
    nlohmann::json config = {
        {"scheduler", {{"endpoint", endpoint}, {"poll_timeout_ms", 5}}},
        {"client", {{"worker_id", "parallel"}, {"heartbeat_interval_ms", 40},
                    {"request_timeout_ms", 500}, {"request_backoff_initial_ms", 10},
                    {"request_backoff_max_ms", 30}, {"deployment_endpoint", "http://fixture"},
                    {"deployment_poll_seconds", 1}}},
        {"plugins", {{"first", {{"enabled", true}, {"dll", "demo.dll"}, {"task_sleep_ms", 800}}},
                     {"second", {{"enabled", true}, {"dll", "client_retry_probe.dll"},
                                 {"process_probe", true}, {"task_sleep_ms", 800}}}}}};
    const auto path = directory / "client.json";
    std::ofstream(path) << config.dump();
    struct State {
        std::string type, session, process;
        int registrations = 0, heartbeats = 0, requests = 0, acknowledgements = 0;
        std::map<std::string, common::TaskResult> reports;
    };
    std::map<std::string, State> states;
    std::map<std::string, common::Task> pending;
    const auto send = [&](const std::string& identity, common::Envelope message) {
        const auto& state = states.at(identity);
        message.set_plugin_type(state.type);
        message.set_session_id(state.session);
        Require(socket->trySendMsgParts({identity, message.SerializeAsString()}), "multi-plugin response blocked");
    };
    const auto poll = [&] {
        if (!socket->waitReadable(10ms)) return;
        const auto frames = socket->recvMsgParts();
        Require(frames.size() == 2, "multi-plugin ROUTER frames invalid");
        common::Envelope message;
        Require(message.ParseFromString(frames[1]), "multi-plugin envelope invalid");
        auto& state = states[frames[0]];
        state.type = message.plugin_type();
        common::Envelope reply;
        switch (message.type()) {
            case common::REGISTER:
                Require(message.register_().physical_client_id() == "parallel",
                        "plugin worker lost its physical deployment identity");
                Require(message.register_().plugin_key() == "first" || message.register_().plugin_key() == "second",
                        "plugin worker omitted its configured plugin key");
                state.session = frames[0] + "-" + std::to_string(++state.registrations);
                state.process = message.register_().payload();
                reply.set_type(common::REGISTER_RESULT);
                reply.mutable_register_result()->set_accepted(true);
                reply.mutable_register_result()->set_plugin_found(true);
                reply.mutable_register_result()->set_registration_id(message.register_().registration_id());
                reply.mutable_register_result()->set_protocol_version(common::kProtocolVersion);
                reply.mutable_register_result()->add_capabilities(common::kAttemptIdsCapability);
                send(frames[0], reply); break;
            case common::HEARTBEAT:
                ++state.heartbeats;
                reply.set_type(common::HEARTBEAT_ACK);
                reply.mutable_heartbeat_ack()->set_registered(message.session_id() == state.session);
                send(frames[0], reply); break;
            case common::REQUEST_TASK: {
                ++state.requests;
                const auto task = pending.find(frames[0]);
                if (task != pending.end()) {
                    reply.set_type(common::TASK);
                    reply.mutable_task()->set_run_id(task->second.run_id());
                    *reply.mutable_task()->add_tasks() = task->second;
                    pending.erase(task);
                } else {
                    reply.set_type(common::NO_TASK); reply.mutable_no_task();
                }
                send(frames[0], reply); break;
            }
            case common::TASK_ACK: ++state.acknowledgements; break;
            case common::TASK_REPORT: {
                const auto& result = message.task_report().result();
                state.reports[result.attempt_id()] = result;
                reply.set_type(common::TASK_REPORT_ACK);
                reply.mutable_task_report_ack()->set_task_id(result.task_id());
                reply.mutable_task_report_ack()->set_attempt_id(result.attempt_id());
                reply.mutable_task_report_ack()->set_accepted(true);
                send(frames[0], reply); break;
            }
            default: break;
        }
    };
    const auto wait = [&](const std::function<bool()>& condition, const char* reason,
                          std::chrono::milliseconds timeout = 6s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!condition() && std::chrono::steady_clock::now() < deadline) poll();
        Require(condition(), reason);
    };
    AgentProcess agent(directory, path.filename());
    wait([&] { return states["parallel/first"].requests && states["parallel/second"].requests; },
         "both plugins did not register/request independently");
    for (const std::string key : {"first", "second"}) {
        auto& task = pending["parallel/" + key];
        task.set_task_id("same-task-id"); task.set_run_id(key + "-run"); task.set_attempt_id(key + "-attempt");
    }
    wait([&] { return states["parallel/first"].acknowledgements && states["parallel/second"].acknowledgements; },
         "both plugins did not accept tasks concurrently");
    Require(states["parallel/first"].reports.empty() && states["parallel/second"].reports.empty(),
            "plugin tasks were serialized instead of running concurrently");
    wait([&] { return states["parallel/first"].reports.size() == 1 && states["parallel/second"].reports.size() == 1; },
         "parallel plugin results did not complete");
    Require(states["parallel/first"].reports.at("first-attempt").run_id() == "first-run" &&
            states["parallel/second"].reports.at("second-attempt").run_id() == "second-run",
            "parallel plugin task/report identities were mixed");

    // Crash the process-probe worker. Its business process must be reaped,
    // while the demo worker retains its live session and heartbeat.
    const auto business_pid = std::stoul(states["parallel/second"].process);
    DWORD worker_pid = 0;
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) do {
        if (entry.th32ProcessID == business_pid) worker_pid = entry.th32ParentProcessID;
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    Require(worker_pid != 0, "cannot find isolated business process owner");
    const auto business = OpenProcess(SYNCHRONIZE, FALSE, business_pid);
    const auto worker = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, worker_pid);
    Require(business && worker && TerminateProcess(worker, 9), "cannot crash second plugin worker");
    CloseHandle(worker);
    const auto first_session = states["parallel/first"].session;
    const auto first_heartbeats = states["parallel/first"].heartbeats;
    wait([&] { return states["parallel/second"].registrations == 2 &&
                     states["parallel/first"].heartbeats > first_heartbeats; },
         "plugin crash did not recover independently");
    Require(states["parallel/first"].session == first_session,
            "plugin crash replaced its healthy sibling's session");
    Require(WaitForSingleObject(business, 1000) == WAIT_OBJECT_0,
            "plugin crash left its business subprocess alive");
    CloseHandle(business);

    const auto offer = [&](char generation, const nlohmann::json& next) {
        const auto target = directory / (std::string("deployment-") + generation + ".json");
        std::ofstream(target) << next.dump();
        std::ofstream(directory / "inbox.json") << nlohmann::json({{"status", "staged"},
            {"revision", std::string(64, generation)}, {"config_path", target.u8string()},
            {"components", nlohmann::json::array()}}).dump();
    };
    const auto active = [&](char generation) {
        try {
            std::ifstream input(directory / "deployments/active.json");
            return nlohmann::json::parse(input).value("revision", "") == std::string(64, generation);
        } catch (...) { return false; }
    };
    auto single = config; single["plugins"]["second"]["enabled"] = false;
    offer('a', single);
    wait([&] { return active('a') && states["parallel"].requests; },
         "multi-plugin deployment could not switch to one compatible worker");
    const auto previous_first = states["parallel/first"].registrations;
    const auto previous_second = states["parallel/second"].registrations;
    offer('b', config);
    wait([&] { return active('b') && states["parallel/first"].registrations > previous_first &&
                                  states["parallel/second"].registrations > previous_second; },
         "multi-plugin deployment did not wait for all new workers to register");
    auto workers = agent.Workers();
    Require(workers.size() == 2, "multi-plugin deployment did not activate two isolated workers");
    agent.Quit();
    for (const auto handle : workers) {
        Require(WaitForSingleObject(handle, 1000) == WAIT_OBJECT_0, "client exit left a plugin worker running");
        CloseHandle(handle);
    }
}

void TestAgentDeployment(const std::filesystem::path& binaries) {
    Peer peer;
    const auto directory = std::filesystem::temp_directory_path() /
        (L"agent-deployment-测试-" + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
    CopyAgentPackage(binaries, directory);
    const auto path = directory / "client.json";
    nlohmann::json config = {
        {"scheduler", {{"endpoint", peer.endpoint}, {"poll_timeout_ms", 5}}},
        {"client", {{"worker_id", "agent"}, {"heartbeat_interval_ms", 40}, {"request_timeout_ms", 500},
                    {"deployment_endpoint", "http://fixture"}, {"deployment_poll_seconds", 1}}},
        {"plugins", {{"demo", {{"enabled", true}, {"dll", "demo.dll"}, {"task_sleep_ms", 1200}}}}}};
    std::ofstream(path) << config.dump();
    const auto staged_path = directory / L"新版本.json";
    std::ofstream(staged_path) << config.dump();
    const auto bad_path = directory / "bad.json";
    auto broken = config;
    broken["plugins"]["demo"]["dll"] = "missing-plugin.dll";
    std::ofstream(bad_path) << broken.dump();
    const auto empty_path = directory / "no-enabled-plugins.json";
    auto empty = config;
    empty["plugins"]["demo"]["enabled"] = false;
    std::ofstream(empty_path) << empty.dump();
    const auto active_file = directory / "deployments/active.json";
    const auto await_file = [&](const std::filesystem::path& file, const std::string& revision) {
        const auto deadline = std::chrono::steady_clock::now() + 6s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (std::filesystem::exists(file)) {
                try { std::ifstream input(file); if (nlohmann::json::parse(input).value("revision", std::string()) == revision) return; }
                catch (const std::exception&) {}
            }
            peer.Pump(30ms);
        }
        throw std::runtime_error("Agent did not publish expected deployment state: " + file.u8string());
    };
    const auto offer = [&](char generation, const std::filesystem::path& target) {
        std::ofstream(directory / "inbox.json") << nlohmann::json({{"status", "staged"},
            {"revision", std::string(64, generation)}, {"config_path", target.u8string()},
            {"components", nlohmann::json::array()}}).dump();
    };
    {
        AgentProcess agent(directory, path.filename());
        peer.Register(peer.Wait(common::REGISTER), "deployment-session");
        Require(peer.identity == "agent", "single worker changed its compatible identity");
        peer.Wait(common::REQUEST_TASK);
        peer.Task("agent-run", "active");
        peer.Wait(common::TASK_ACK);
        offer('a', staged_path);
        peer.Wait(common::TASK_REPORT, 4s);
        peer.Pump(300ms);
        Require(!std::filesystem::exists(active_file), "Agent activated before task reports were acknowledged");
        peer.Ack("active", true, false);
        auto registration = peer.Wait(common::REGISTER, 5s);
        Require(!std::filesystem::exists(active_file), "Agent promoted a release before worker registration");
        peer.Register(registration, "deployment-session");
        await_file(active_file, std::string(64, 'a'));
        // An invalid prepared release must be rejected before draining or
        // replacing the healthy single-plugin worker, even if a helper stages it.
        offer('b', empty_path);
        await_file(directory / "deployments/failed.json", std::string(64, 'b'));
        await_file(active_file, std::string(64, 'a'));
        const auto changed = peer.Receive([](const common::Envelope& envelope) {
            return envelope.type() == common::REGISTER;
        }, 200ms);
        Require(changed.type() != common::REGISTER, "empty plugin release replaced the healthy worker");
        offer('c', bad_path);
        registration = peer.Wait(common::REGISTER, 6s);
        peer.Register(registration, "deployment-session");
        await_file(directory / "deployments/failed.json", std::string(64, 'c'));
        await_file(active_file, std::string(64, 'a'));
        const auto repeated = peer.Receive([](const common::Envelope& envelope) { return envelope.type() == common::REGISTER; }, 2200ms);
        Require(repeated.type() != common::REGISTER, "failed generation was repeatedly retried");
        agent.Quit();
    }
    // Restart must select the committed active config rather than the now-invalid base.
    std::ofstream(path) << broken.dump();
    AgentProcess restarted(directory, path.filename());
    peer.Register(peer.Wait(common::REGISTER, 4s), "deployment-session");
    peer.Wait(common::REQUEST_TASK);
    peer.Pump(100ms);
    {
        std::ifstream input(directory / "deployments/agent_report.json");
        const auto report = nlohmann::json::parse(input);
        Require(report.value("state", std::string()) == "failed" &&
                    report.value("desired_revision", std::string()) == std::string(64, 'c'),
                "Agent restart discarded the failed deployment report awaiting delivery");
    }
    restarted.Quit();
}

void TestAgentLateUpdaterResults(const std::filesystem::path& binaries) {
    Peer peer;
    const auto directory = std::filesystem::temp_directory_path() /
        (L"agent-late-updater-" + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
    CopyAgentPackage(binaries, directory);
    const auto config_path = directory / "client.json";
    const nlohmann::json config = {
        {"scheduler", {{"endpoint", peer.endpoint}, {"poll_timeout_ms", 5}}},
        {"client", {{"worker_id", "agent"}, {"heartbeat_interval_ms", 40}, {"request_timeout_ms", 500},
                    {"deployment_endpoint", "http://fixture"}, {"deployment_poll_seconds", 1}}},
        {"plugins", {{"demo", {{"enabled", true}, {"dll", "demo.dll"}}}}}};
    std::ofstream(config_path) << config.dump();
    const auto active_file = directory / "deployments/active.json";
    const auto report_file = directory / "deployments/agent_report.json";
    const auto wait_json = [&](const std::filesystem::path& file,
                               const std::function<bool(const nlohmann::json&)>& matches) {
        const auto deadline = std::chrono::steady_clock::now() + 8s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (std::filesystem::exists(file)) {
                try {
                    std::ifstream input(file);
                    const auto value = nlohmann::json::parse(input);
                    if (matches(value)) return value;
                } catch (const std::exception&) {}
            }
            peer.Pump(20ms);
        }
        throw std::runtime_error("Agent did not reach updater test boundary: " + file.u8string());
    };
    const auto publish = [&](const nlohmann::json& result) {
        const auto temporary = directory / "inbox.tmp";
        std::ofstream(temporary) << result.dump();
        Require(MoveFileExW(temporary.c_str(), (directory / "inbox.json").c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), "cannot publish updater fixture");
    };
    AgentProcess agent(directory, config_path.filename());
    peer.Register(peer.Wait(common::REGISTER), "late-updater-session");
    peer.Pump(50ms);
    std::string previous;
    char generation = 'a';
    for (const std::string late_status : {"waiting_window", "failed", "staged"}) {
        const auto revision = std::string(64, generation++);
        const auto token = "late-" + late_status;
        publish({{"status", "staged"}, {"revision", revision}, {"config_path", config_path.u8string()},
                 {"components", nlohmann::json::array({{{"slot", "plugin.demo"},
                     {"artifact_id", std::string(32, 'a')}, {"state", "staged"}}})},
                 {"message", "delayed preparation result"}, {"test_late_status", late_status}, {"test_token", token}});
        const auto registration = peer.Wait(common::REGISTER, 6s);
        // Hold registration until a helper has captured the not-yet-active
        // generation, then let activation finish before its result can return.
        wait_json(directory / (token + ".started.json"), [&](const nlohmann::json& value) {
            return value.value("current", std::string()) == previous &&
                   value.value("prepared", std::string()) == revision;
        });
        peer.Register(registration, "late-updater-session");
        wait_json(active_file, [&](const nlohmann::json& value) {
            return value.value("revision", std::string()) == revision;
        });
        const auto ready = wait_json(report_file, [&](const nlohmann::json& value) {
            return value.value("desired_revision", std::string()) == revision &&
                   value.value("state", std::string()) == "ready";
        });
        Require(ready.at("components").at(0).at("state") == "active", "ready report lost active component state");
        std::ofstream(directory / (token + ".release")) << "release";
        // Starting the next helper proves that the Agent consumed the delayed
        // result. Its current shortcut must not be needed to repair a downgrade.
        wait_json(directory / (token + ".current.json"), [&](const nlohmann::json& value) {
            return value.value("revision", std::string()) == revision;
        });
        std::ifstream input(report_file);
        Require(nlohmann::json::parse(input) == ready, "late updater result downgraded an active deployment report");
        Require(!std::filesystem::exists(directory / "deployments/failed.json"),
                "late preparation failure rejected the already active deployment");
        previous = revision;
    }
    // Reports for a genuinely different desired generation must still be visible.
    const auto next_revision = std::string(64, 'd');
    for (const std::string status : {"waiting_window", "failed"}) {
        publish({{"status", status}, {"revision", next_revision}, {"message", "next deployment preparation"},
                 {"components", nlohmann::json::array()}});
        wait_json(report_file, [&](const nlohmann::json& value) {
            return value.value("desired_revision", std::string()) == next_revision &&
                   value.value("state", std::string()) == status;
        });
        std::ifstream input(active_file);
        Require(nlohmann::json::parse(input).at("revision") == previous,
                "unactivated desired generation replaced the running deployment");
    }
    agent.Quit();
}
#endif
}  // namespace

int main(int argc, char** argv) {
    try {
        const auto binary_directory = std::filesystem::absolute(argv[0]).parent_path();
        // Protobuf's generated descriptor registry outlives plugin unloads.
        // Hold the demo DLL across the in-process protocol cases; production
        // updates instead replace the entire worker process.
#ifdef _WIN32
        const auto demo_guard = common::DynamicLibrary::load((binary_directory / "demo.dll").u8string());
#else
        const auto demo_guard = common::DynamicLibrary::load((binary_directory / "libdemo.so").u8string());
#endif
        TestClient(binary_directory);
        TestInitialRegistrationRetryLimit(binary_directory);
        TestDeploymentDrain(binary_directory);
#ifdef _WIN32
        TestAgentProcesses(binary_directory);
        TestAgentMultiplePlugins(binary_directory);
        TestAgentDeployment(binary_directory);
        TestAgentLateUpdaterResults(binary_directory);
#endif
        std::cout << "PASS: report retries, stale ACK, STOP heartbeat, re-registration, protocol "
                     "rejection, registration retry limit, graceful deployment drain, Agent process "
                     "isolation/restart/cleanup, parallel plugins, one/many plugin deployment switching, "
                     "readiness-gated activation, late updater reports and rollback\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
