#include "scheduler/Scheduler.h"
#include "common/Protocol.h"
#include "common/AtomicFile.h"
#include "common/Util.h"
#include "common/zmq_helper.h"
#include <json.hpp>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
using namespace std::chrono_literals;
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

struct Workspace {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("scheduler-protocol-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Workspace() { std::filesystem::create_directory(root); }
    ~Workspace() { std::error_code error; std::filesystem::remove_all(root,error); }
};

class Peer {
public:
    Peer(const std::string& endpoint, const std::string& identity="test-worker",
         std::string type="scheduler-probe") : dealer_(endpoint), type_(std::move(type)) {
        dealer_.setOption(ZMQ_IDENTITY, identity.data(), identity.size());
        dealer_.connect();
    }
    void Send(common::Envelope envelope) {
        SendAs(std::move(envelope), session);
    }
    void SendAs(common::Envelope envelope, const std::string& session_id) {
        envelope.set_plugin_type(type_);
        envelope.set_session_id(session_id);
        dealer_.sendMsg(envelope.SerializeAsString());
    }
    common::Envelope Wait(common::MessageType type, std::chrono::milliseconds timeout=2s) {
        const auto until=std::chrono::steady_clock::now()+timeout;
        while (std::chrono::steady_clock::now()<until) {
            if (!dealer_.waitReadable(10ms)) continue;
            common::Envelope envelope;
            Require(envelope.ParseFromString(dealer_.recvMsg()),"invalid server envelope");
            if (envelope.type()==type) return envelope;
        }
        throw std::runtime_error("missing server message " + std::to_string(type));
    }
    common::Envelope Register(const std::string& nonce,
                              unsigned int protocol = common::kProtocolVersion,
                              bool capability = true, const std::string& physical_client = {},
                              const std::string& plugin_key = {}, const std::string& deployment_target = {}) {
        common::Envelope request; request.set_type(common::REGISTER);
        request.mutable_register_()->set_registration_id(nonce);
        request.mutable_register_()->set_protocol_version(protocol);
        request.mutable_register_()->set_physical_client_id(physical_client);
        request.mutable_register_()->set_plugin_key(plugin_key);
        request.mutable_register_()->set_deployment_target_id(deployment_target);
        if (capability) request.mutable_register_()->add_capabilities(common::kAttemptIdsCapability);
        Send(request);
        const auto result=Wait(common::REGISTER_RESULT);
        if (protocol == common::kProtocolVersion && capability)
            Require(result.register_result().accepted(),"registration rejected");
        session=result.session_id();
        return result;
    }
    common::Task Request() {
        common::Envelope request; request.set_type(common::REQUEST_TASK);
        request.mutable_request_task()->set_count(1); Send(request);
        return Wait(common::TASK).task().tasks(0);
    }
    void AckAs(const common::Task& task, const std::string& attempt,
               const std::string& session_id) {
        common::Envelope ack; ack.set_type(common::TASK_ACK);
        ack.mutable_task_ack()->set_task_id(task.task_id());
        ack.mutable_task_ack()->set_attempt_id(attempt); SendAs(ack, session_id);
    }
    void Report(const common::Task& task, const std::string& attempt, int exit_code=0) {
        ReportAs(task, attempt, session, exit_code);
    }
    void ReportAs(const common::Task& task, const std::string& attempt,
                  const std::string& session_id, int exit_code=0) {
        common::Envelope report; report.set_type(common::TASK_REPORT);
        auto* result=report.mutable_task_report()->mutable_result();
        result->set_task_id(task.task_id()); result->set_run_id(task.run_id());
        result->set_attempt_id(attempt); result->set_exit_code(exit_code); SendAs(report, session_id);
    }
    void Heartbeat(common::Heartbeat::Status status = common::Heartbeat::IDLE) {
        common::Envelope message; message.set_type(common::HEARTBEAT);
        message.mutable_heartbeat()->set_status(status); Send(message);
    }
    std::string session;
private:
    common::ZmqDealer dealer_;
    std::string type_;
};

void Test(const std::filesystem::path& bin) {
    Workspace workspace;
    auto& config=common::Config::instance();
    const auto entered=workspace.root/"entered";
    const auto config_file=workspace.root/"scheduler.json";
    // Bind trial ports inside the real server, retrying collisions without
    // assuming any existing service belongs to this test.
    std::unique_ptr<scheduler::Scheduler> server;
    std::string endpoint;
    for (int attempt=0;attempt<50;++attempt) {
        const auto port=30000 + (std::chrono::steady_clock::now().time_since_epoch().count()%20000);
        endpoint="tcp://127.0.0.1:"+std::to_string(port);
        nlohmann::json settings={
            {"server",{{"router_endpoint",endpoint},{"poll_timeout_ms",5},{"client_timeout_ms",0},
                       {"task_ack_timeout_ms",250}}},
            {"plugins",{{"probe",{{"enabled",true},{"dll",(bin/"scheduler_probe.dll").string()},
                                   {"entered_file",entered.string()}}}}}};
        std::ofstream(config_file) << settings.dump();
        config.load(config_file.string());
        server=std::make_unique<scheduler::Scheduler>(config);
        try { server->start(); break; }
        catch (...) { server.reset(); }
    }
    Require(server!=nullptr,"could not bind private scheduler test port");
    Peer peer(endpoint);
    const auto incompatible = peer.Register("old-client", 0, false);
    Require(!incompatible.register_result().accepted() && incompatible.session_id().empty() &&
            incompatible.register_result().reason().find("incompatible client protocol") !=
                std::string::npos,
            "incompatible client was registered without an explicit reason");
    Require(server->clients().empty(), "rejected client retained a registered session");
    const auto future_protocol = peer.Register("future-client", common::kProtocolVersion + 1);
    Require(!future_protocol.register_result().accepted() && future_protocol.session_id().empty(),
            "unknown protocol version was accepted");
    const auto missing_capability = peer.Register("no-attempt-ids", common::kProtocolVersion, false);
    Require(!missing_capability.register_result().accepted() && missing_capability.session_id().empty(),
            "missing required capability was accepted");
    const auto registration=peer.Register("nonce-one");
    Require(peer.Register("nonce-one").session_id()==registration.session_id(),
            "registration retry replaced a valid session");
    Require(server->createRun("scheduler-probe","dataset",""),"run creation failed");
    const auto task=peer.Request();
    peer.Report(task,"obsolete");
    Require(!peer.Wait(common::TASK_REPORT_ACK).task_report_ack().accepted(),"stale attempt accepted");
    Require(server->inFlightCount()==1,"stale attempt removed current task");

    const auto old_session=peer.session;
    const auto replacement_registration=peer.Register("nonce-two");
    Require(replacement_registration.session_id()!=old_session,
            "new registration_id reused the old session");
    const auto replacement=peer.Request();
    Require(replacement.task_id()==task.task_id(),"takeover did not requeue the old task");
    Require(replacement.attempt_id()!=task.attempt_id(),
            "takeover reused the old assignment attempt");

    // A replaced session must not acknowledge or finish the new session's attempt.
    peer.AckAs(replacement,replacement.attempt_id(),old_session);
    peer.ReportAs(replacement,replacement.attempt_id(),old_session);
    Require(!peer.Wait(common::TASK_REPORT_ACK).task_report_ack().accepted(),
            "old session report changed the replacement attempt");
    Require(server->inFlightCount()==1,"old session removed the replacement attempt");
    const auto reclaim_deadline=std::chrono::steady_clock::now()+2s;
    while (server->inFlightCount()!=0 && std::chrono::steady_clock::now()<reclaim_deadline)
        std::this_thread::sleep_for(5ms);
    Require(server->inFlightCount()==0,"old session ACK kept the replacement attempt alive");

    const auto retry=peer.Request();
    Require(retry.task_id()==task.task_id(),"new session did not receive the reclaimed task");
    Require(retry.attempt_id()!=replacement.attempt_id(),
            "reclaimed task reused the previous attempt");
    peer.AckAs(retry,retry.attempt_id(),peer.session);
    peer.Report(retry,retry.attempt_id());
    const auto deadline=std::chrono::steady_clock::now()+2s;
    while (!std::filesystem::exists(entered) && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(5ms);
    Require(std::filesystem::exists(entered),"slow result callback did not start");
    const auto heartbeat_start=std::chrono::steady_clock::now();
    peer.Heartbeat();
    Require(peer.Wait(common::HEARTBEAT_ACK,300ms).heartbeat_ack().registered(),
            "heartbeat lost registered session");
    Require(std::chrono::steady_clock::now()-heartbeat_start<300ms,
            "plugin callback blocked heartbeat");
    Require(peer.Wait(common::TASK_REPORT_ACK).task_report_ack().accepted(),"valid result rejected");
    peer.Report(retry,retry.attempt_id());
    Require(peer.Wait(common::TASK_REPORT_ACK).task_report_ack().accepted(),"duplicate report lost ACK");

    // STOP waits behind previous callbacks. No callback can touch the new run.
    Require(server->stopRun("scheduler-probe"),"stop did not finish");
    Require(server->createRun("scheduler-probe","next",""),"stop then run was reordered");
    server->wakeupClients("scheduler-probe");
    const auto next=peer.Request();
    peer.Report(retry,retry.attempt_id());
    peer.Wait(common::TASK_REPORT_ACK);
    Require(server->inFlightCount()==1,"old report changed the replacement run");
    peer.AckAs(next,next.attempt_id(),peer.session);
    peer.Report(next,next.attempt_id());
    Require(peer.Wait(common::TASK_REPORT_ACK).task_report_ack().accepted(),"new run did not complete");

    const auto finish = [&](int exit_code) {
        const auto assigned = peer.Request();
        peer.AckAs(assigned,assigned.attempt_id(),peer.session);
        peer.Report(assigned,assigned.attempt_id(),exit_code);
        Require(peer.Wait(common::TASK_REPORT_ACK).task_report_ack().accepted(),"business outcome rejected");
        const auto until = std::chrono::steady_clock::now()+2s;
        while (server->activeRunCount()!=0 && std::chrono::steady_clock::now()<until)
            std::this_thread::sleep_for(5ms);
        Require(server->activeRunCount()==0,"business outcome did not finish the run");
        return assigned.run_id();
    };
    const nlohmann::json command={{"action","run"},{"plugin_type","scheduler-probe"},
                                  {"dataset_version","business-failure"}};
    Require(server->managementCommand(command).value("ok",false),"failed run could not start");
    const auto failed = finish(1);
    auto forced = command;
    forced["force_rerun"] = true;
    forced["force_rerun_reason"] = "diagnose failed execution";
    const auto forced_failure = server->managementCommand(forced);
    Require(forced_failure.value("ok",false) && forced_failure.value("rerun_of",std::string())==failed,
            "forced rerun lost its failed predecessor without framework storage");
    finish(1);
    Require(server->managementCommand(command).value("ok",false),
            "business failure blocked ordinary retry without framework storage");
    const auto successful = finish(0);
    Require(server->managementCommand(forced).value("ok",false),"forced verification could not start");
    finish(1);
    const auto duplicate = server->managementCommand(command);
    Require(duplicate.value("duplicate",false) && duplicate.value("run_id",std::string())==successful,
            "latest failed run hid an earlier complete success without framework storage");
    server->stop();
}

void TestParallelPlugins(const std::filesystem::path& bin) {
    Workspace workspace;
    auto& config = common::Config::instance();
    const auto config_file = workspace.root / "scheduler.json";
    std::unique_ptr<scheduler::Scheduler> server;
    std::string endpoint;
    for (int attempt = 0; attempt < 50; ++attempt) {
        const auto port = 30000 + (std::chrono::steady_clock::now().time_since_epoch().count() % 20000);
        endpoint = "tcp://127.0.0.1:" + std::to_string(port);
        nlohmann::json settings = {
            {"server", {{"router_endpoint", endpoint}, {"poll_timeout_ms", 5},
                        {"client_timeout_ms", 0}, {"task_ack_timeout_ms", 10000}}},
            {"plugins", nlohmann::json::object()}};
        for (const std::string type : {"scheduler-probe", "other-probe"}) {
            settings["plugins"][type] = {{"enabled", true},
                {"dll", (bin / "scheduler_probe.dll").string()}, {"plugin_type", type},
                {"entered_file", (workspace.root / (type + ".entered")).string()}};
        }
        std::ofstream(config_file) << settings.dump();
        config.load(config_file.string());
        server = std::make_unique<scheduler::Scheduler>(config);
        try { server->start(); break; } catch (...) { server.reset(); }
    }
    Require(server != nullptr, "could not bind parallel scheduler test port");
    Peer first(endpoint, "machine/first");
    Peer second(endpoint, "machine/second", "other-probe");
    Peer outside(endpoint, "other/first");
    first.Register("first", common::kProtocolVersion, true, "machine", "first");
    second.Register("second", common::kProtocolVersion, true, "machine", "second");
    outside.Register("outside", common::kProtocolVersion, true, "other", "first");
    const auto enqueue = [&](const char* type, const char* dataset) {
        return server->managementCommand({{"action", "run"}, {"plugin_type", type},
            {"dataset_version", dataset}, {"worker_ids", {"machine"}}});
    };
    const auto first_run = enqueue("scheduler-probe", "first-dataset");
    const auto second_run = enqueue("other-probe", "first-dataset");
    const auto first_queued = enqueue("scheduler-probe", "next-dataset");
    const auto second_queued = enqueue("other-probe", "next-dataset");
    Require(first_run.value("status", "") == "running" && second_run.value("status", "") == "running",
            "different plugins did not activate simultaneously");
    Require(first_queued.value("status", "") == "queued" && second_queued.value("status", "") == "queued",
            "each plugin must retain its own ordered run queue");
    common::Envelope request; request.set_type(common::REQUEST_TASK);
    request.mutable_request_task()->set_count(1);
    outside.Send(request);
    outside.Wait(common::NO_TASK);
    const auto one = first.Request();
    const auto two = second.Request();
    first.AckAs(one, one.attempt_id(), first.session);
    second.AckAs(two, two.attempt_id(), second.session);
    Require(one.run_id() == first_run["run_id"] && two.run_id() == second_run["run_id"] &&
            server->inFlightCount() == 2, "physical client selection lost parallel plugin assignments");
    first.Report(two, two.attempt_id());
    Require(!first.Wait(common::TASK_REPORT_ACK).task_report_ack().accepted() && server->inFlightCount() == 2,
            "one plugin worker accepted another plugin's task report");
    first.Report(one, one.attempt_id());
    Require(first.Wait(common::TASK_REPORT_ACK).task_report_ack().accepted(), "first parallel report rejected");
    const auto next_one = first.Request();
    Require(next_one.run_id() == first_queued["run_id"], "first plugin did not advance its own run queue");
    first.AckAs(next_one, next_one.attempt_id(), first.session);
    second.Report(two, two.attempt_id());
    Require(second.Wait(common::TASK_REPORT_ACK).task_report_ack().accepted(), "second parallel report rejected");
    const auto next_two = second.Request();
    Require(next_two.run_id() == second_queued["run_id"], "second plugin did not advance its own run queue");
    second.AckAs(next_two, next_two.attempt_id(), second.session);
    const auto status = server->managementStatus();
    bool mapped_first = false, mapped_second = false;
    for (const auto& client : status["clients"]) {
        if (client["worker_id"] == "machine/first")
            mapped_first = client["physical_client_id"] == "machine" && client["plugin_key"] == "first";
        if (client["worker_id"] == "machine/second")
            mapped_second = client["physical_client_id"] == "machine" && client["plugin_key"] == "second";
    }
    Require(mapped_first && mapped_second, "management status lost physical client/plugin mapping");
    Require(server->stopRun("scheduler-probe"), "stopping one plugin failed");
    Require(server->activeRunCount() == 1 && server->inFlightCount() == 1,
            "stopping one plugin interrupted the sibling plugin");
    second.Report(next_two, next_two.attempt_id());
    Require(second.Wait(common::TASK_REPORT_ACK).task_report_ack().accepted(),
            "sibling report failed after another plugin stopped");
    Peer single(endpoint, "machine");
    single.Register("single", common::kProtocolVersion, true, "machine", "first");
    const auto after_switch = server->managementCommand({{"action", "run"},
        {"plugin_type", "scheduler-probe"}, {"dataset_version", "single-plugin-again"},
        {"worker_ids", {"machine/first"}}});
    Require(after_switch.value("ok", false), "run targeting the logical plugin worker was rejected");
    const auto single_task = single.Request();
    Require(single_task.run_id() == after_switch["run_id"],
            "returning to one plugin lost the previously selected plugin worker");
    single.Report(single_task, single_task.attempt_id());
    Require(single.Wait(common::TASK_REPORT_ACK).task_report_ack().accepted(),
            "single worker result after multi-plugin deployment was rejected");
    server->stop();
}

void TestOnlineDeployment(const std::filesystem::path& bin) {
    Workspace workspace;
    auto& config = common::Config::instance();
    const auto config_directory = workspace.root /
        std::filesystem::u8path(u8"\u4e2d\u6587\u914d\u7f6e\u76ee\u5f55");
    std::filesystem::create_directory(config_directory);
    const auto config_file = config_directory / "scheduler.json";
#ifdef _WIN32
    const auto constructor_path = [&]() {
        const auto wide = config_file.wstring();
        const int length = WideCharToMultiByte(CP_ACP, 0, wide.c_str(), static_cast<int>(wide.size()),
                                               nullptr, 0, nullptr, nullptr);
        std::string result(static_cast<size_t>(length > 0 ? length : 0), '\0');
        if (length) WideCharToMultiByte(CP_ACP, 0, wide.c_str(), static_cast<int>(wide.size()),
                                        result.data(), length, nullptr, nullptr);
        const int roundtrip_length = MultiByteToWideChar(CP_ACP, 0, result.data(), length, nullptr, 0);
        std::wstring roundtrip(static_cast<size_t>(roundtrip_length > 0 ? roundtrip_length : 0), L'\0');
        if (roundtrip_length) MultiByteToWideChar(CP_ACP, 0, result.data(), length,
                                                roundtrip.data(), roundtrip_length);
        if (roundtrip != wide) return config_file.u8string();
        return result;
    }();
#else
    const auto constructor_path = config_file.u8string();
#endif
    std::unique_ptr<scheduler::Scheduler> server;
    std::string endpoint;
    for (int attempt = 0; attempt < 50; ++attempt) {
        const auto port = 30000 + (std::chrono::steady_clock::now().time_since_epoch().count() % 20000);
        endpoint = "tcp://127.0.0.1:" + std::to_string(port);
        nlohmann::json settings = {
            {"server", {{"router_endpoint", endpoint}, {"poll_timeout_ms", 5},
                        {"client_timeout_ms", 0}, {"task_ack_timeout_ms", 10000}}},
            {"plugins", nlohmann::json::object()}};
        for (const std::string type : {"scheduler-probe", "other-probe"})
            settings["plugins"][type] = {{"enabled", true},
                {"dll", (bin / "scheduler_probe.dll").string()}, {"plugin_type", type},
                {"expected_program_version", "v1"},
                {"entered_file", (workspace.root / (type + ".entered")).string()}};
        std::ofstream(config_file) << settings.dump();
        config.load(config_file.u8string());
        // Exercise native ANSI compatibility where the system code page can
        // represent this path; otherwise retain full Unicode coverage in UTF-8.
        server = std::make_unique<scheduler::Scheduler>(config, constructor_path);
        try { server->start(); break; } catch (...) { server.reset(); }
    }
    Require(server != nullptr, "could not bind online deployment scheduler test port");
    Peer first(endpoint, "machine/first"), sibling(endpoint, "machine/second", "other-probe");
    first.Register("first-registration", common::kProtocolVersion, true, "machine", "first");
    sibling.Register("sibling-registration", common::kProtocolVersion, true, "machine", "second");
    Require(server->managementCommand({{"action", "agent_heartbeat"}, {"client_id", "machine"},
        {"instance_id", "agent-instance"}, {"hostname", "test-host"}, {"state", "ready"},
        {"plugin_keys", {"scheduler-probe", "other-probe"}}, {"agent_version", "test"},
        {"active_revision", "old"}, {"update_window_open", true}}).value("ok", false),
        "online deployment agent registration failed");
    Require(server->createRun("other-probe", "d1", ""), "sibling run failed before online deployment");
    const auto task = sibling.Request();
    sibling.AckAs(task, task.attempt_id(), sibling.session);
    std::string original;
    Require(common::ReadCompleteFile(config_file, &original), "cannot read original deployment config");
    const auto old_session = first.session;
    const auto sibling_session = sibling.session;
    const auto instance = server->managementStatus()["instance_id"];
    const auto apply = [&](const std::string& revision, const char* type, const char* version) {
        return server->managementCommand({{"action", "apply_plugin_deployment"},
            {"config_path", config_file.u8string()}, {"config_revision", revision},
            {"plugins", {{type, {{"expected_program_version", version}}}}}});
    };
    const auto applied = apply(common::Sha256Hex(original), "scheduler-probe", "v2");
    Require(applied.value("ok", false) && applied.value("online_applied", false),
            "idle plugin deployment was rejected while sibling was busy");
    std::string saved, backup;
    auto backup_file = config_file;
    backup_file += ".bak";
    Require(common::ReadCompleteFile(config_file, &saved) &&
            common::ReadCompleteFile(backup_file, &backup) && backup == original &&
            applied["configuration_revision"] == common::Sha256Hex(saved),
            "online deployment did not durably publish its configuration and exact backup");
    const auto status = server->managementStatus();
    Require(status["config_path"] == config_file.u8string() &&
            std::filesystem::exists(config_directory / "runtime" / "client_control.json"),
            "Unicode configuration path was not normalized to UTF-8");
    Require(status["instance_id"] == instance && status["clients"].size() == 1 &&
            status["clients"][0]["worker_id"] == "machine/second" && status["agents"].size() == 1 &&
            status["in_flight"] == 1 && status["active_runs"].size() == 1,
            "online deployment changed another plugin's session, work or physical Agent");
    Require(status["configuration"]["plugins"]["scheduler-probe"]["expected_program_version"] == "v2" &&
            config.get("plugins.scheduler-probe.expected_program_version", std::string()) == "v1",
            "online deployment did not publish its snapshot independently of startup Config");
    first.Heartbeat();
    Require(!first.Wait(common::HEARTBEAT_ACK).heartbeat_ack().registered(),
            "obsolete deployment session remained registered");
    first.Register("first-registration", common::kProtocolVersion, true, "machine", "first");
    Require(first.session != old_session, "cached registration replay restored an obsolete session");
    sibling.Heartbeat();
    Require(sibling.Wait(common::HEARTBEAT_ACK).heartbeat_ack().registered() && sibling.session == sibling_session,
            "online deployment invalidated the sibling plugin session");
    Require(apply(common::Sha256Hex(saved), "other-probe", "v2").value("http_status", 0) == 409,
            "online deployment modified a busy plugin");
    sibling.Report(task, task.attempt_id());
    Require(sibling.Wait(common::TASK_REPORT_ACK).task_report_ack().accepted(),
            "sibling task could not finish after online deployment");
    Require(server->createRun("scheduler-probe", "identity-check", ""), "updated plugin could not run");
    const auto first_task = first.Request();
    first.Report(first_task, first_task.attempt_id());
    Require(first.Wait(common::TASK_REPORT_ACK).task_report_ack().accepted(), "updated plugin report failed");
    // CommitFinished is included in a subsequent command barrier when needed.
    server->managementCommand({{"action", "run"}, {"plugin_type", "scheduler-probe"},
                               {"dataset_version", "identity-check"}});
    const auto again = apply(common::Sha256Hex(saved), "scheduler-probe", "v3");
    Require(again.value("ok", false), "finished work prevented another deployment");
    Require(server->createRun("scheduler-probe", "identity-check", ""),
            "updated deployment identity was incorrectly deduplicated against its predecessor");
    server->stop();
}

void TestClientControl(const std::filesystem::path& bin) {
    Workspace workspace;
    auto& config = common::Config::instance();
    const auto config_file = workspace.root / "scheduler.json";
    const auto state_file = workspace.root / "runtime" / "client_control.json";
    std::unique_ptr<scheduler::Scheduler> server;
    std::string endpoint;
    for (int attempt = 0; attempt < 50; ++attempt) {
        const auto port = 30000 + (std::chrono::steady_clock::now().time_since_epoch().count() % 20000);
        endpoint = "tcp://127.0.0.1:" + std::to_string(port);
        nlohmann::json settings = {
            {"server", {{"router_endpoint", endpoint}, {"poll_timeout_ms", 5},
                        {"client_timeout_ms", 0}, {"task_ack_timeout_ms", 10000},
                        {"deployment_reservation_timeout_ms", 1000}}},
            {"plugins", nlohmann::json::object()}};
        for (const std::string type : {"scheduler-probe", "other-probe"})
            settings["plugins"][type] = {{"enabled", true},
                {"dll", (bin / "scheduler_probe.dll").string()}, {"plugin_type", type},
                {"entered_file", (workspace.root / (type + ".entered")).string()}};
        std::ofstream(config_file) << settings.dump();
        config.load(config_file.string());
        server = std::make_unique<scheduler::Scheduler>(config, config_file.string());
        try { server->start(); break; } catch (...) { server.reset(); }
    }
    Require(server != nullptr, "could not bind client control scheduler test port");
    const auto heartbeat = [&](const char* id, const char* state = "ready", bool update_window_open = true) {
        return server->managementCommand({{"action", "agent_heartbeat"}, {"client_id", id},
            {"instance_id", "agent-instance"}, {"hostname", "test-host"}, {"state", state},
            {"plugin_keys", nlohmann::json::array()}, {"agent_version", "test"},
            {"active_revision", ""}, {"update_window_open", update_window_open}});
    };
    const auto control = [&](const char* action, const char* token, nlohmann::json ids) {
        return server->managementCommand({{"action", action}, {"reservation_id", token}, {"client_ids", ids}});
    };
    const auto agent = [&](const char* id) -> nlohmann::json {
        const auto status = server->managementStatus();
        for (const auto& item : status["agents"])
            if (item["client_id"] == id) return item;
        throw std::runtime_error("agent missing from management status");
    };
    Require(heartbeat("empty", "unconfigured").value("ok", false), "empty agent registration failed");
    Require(server->clients().empty() && agent("empty")["online"] == true &&
            agent("empty")["state"] == "unconfigured", "empty agent was not independently discoverable");
    Require(server->managementStatus()["client_control_path"] == state_file.string(),
            "client control state did not use the config directory");
    Require(server->managementCommand({{"action", "agent_heartbeat"}, {"client_id", "bad"}})
                .value("http_status", 0) == 400, "malformed agent heartbeat accepted");
    Require(control("reserve_clients", "token", {"unknown"}).value("http_status", 0) == 409,
            "unknown agent reserved");

    Peer first(endpoint, "machine/first"), second(endpoint, "machine/second", "other-probe");
    first.Register("first", common::kProtocolVersion, true, "machine", "first");
    second.Register("second", common::kProtocolVersion, true, "machine", "second");
    Require(heartbeat("machine").value("ok", false), "configured agent registration failed");
    first.Heartbeat(common::Heartbeat::BUSY);
    first.Wait(common::HEARTBEAT_ACK);
    const auto heartbeat_busy = control("reserve_clients", "token", {"empty", "machine"});
    Require(heartbeat_busy.value("http_status", 0) == 409 &&
            heartbeat_busy["unavailable"][0]["reason"] == "busy" && agent("empty")["reservation_id"] == "",
            "busy worker heartbeat did not atomically reject the whole batch");
    first.Heartbeat(); first.Wait(common::HEARTBEAT_ACK);
    Require(server->createRun("scheduler-probe", "control-first", ""), "control first run failed");
    const auto task = first.Request();
    first.AckAs(task, task.attempt_id(), first.session);
    Require(control("reserve_clients", "token", {"empty", "machine"}).value("http_status", 0) == 409,
            "in-flight task did not block physical client reservation");
    Require(server->stopRun("scheduler-probe"), "control task did not stop");

    Require(heartbeat("machine", "starting", false).value("ok", false), "startup repair heartbeat rejected");
    const auto outside_window = server->managementCommand({{"action", "reserve_clients"},
        {"reservation_id", "rollout-window"}, {"client_ids", {"empty", "machine"}}, {"require_update_window_open", true}});
    Require(outside_window.value("http_status", 0) == 409 && outside_window["unavailable"][0]["reason"] == "waiting_window" &&
        agent("empty")["reservation_id"] == "" && agent("machine")["reservation_id"] == "",
        "rollout reservation ignored the latest update window or partially reserved the batch");
    Require(control("reserve_clients", "token", {"empty", "machine"}).value("ok", false),
            "idle starting clients could not receive a repairing deployment outside their update window");
    Require(control("reserve_clients", "other-token", {"empty", "machine"}).value("http_status", 0) == 409,
            "another publisher replaced reservations");
    Require(heartbeat("machine", "updating", false).value("ok", false), "update heartbeat rejected");
    Require(control("reserve_clients", "token", {"empty", "machine"}).value("ok", false),
            "same-token recovery was blocked by its own updating state");
    Require(control("commit_reservation", "token", {"empty", "machine"}).value("ok", false),
            "reservation commit failed");
    Require(control("release_clients", "wrong-token", {"machine"}).value("http_status", 0) == 409,
            "wrong token released a deployment reservation");
    Require(server->createRun("scheduler-probe", "blocked-first", "") &&
            server->createRun("other-probe", "blocked-second", ""), "reserved run planning failed");
    common::Envelope request; request.set_type(common::REQUEST_TASK);
    request.mutable_request_task()->set_count(1);
    first.Send(request); first.Wait(common::NO_TASK);
    second.Send(request); second.Wait(common::NO_TASK);
    Require(server->inFlightCount() == 0 && agent("machine")["update_window_open"] == false,
            "machine awaiting its update window received plugin tasks");
    std::this_thread::sleep_for(1050ms);
    Require(control("reserve_clients", "token", {"machine"}).value("ok", false) &&
            agent("machine")["reservation_committed"] == true,
            "committed reservation expired with its pending lease");
    server->stop();
    server.reset();

    server = std::make_unique<scheduler::Scheduler>(config, config_file.string());
    server->start();
    Require(agent("empty")["online"] == false && agent("empty")["state"] == "unconfigured" &&
            agent("machine")["reservation_id"] == "token",
            "restart lost offline agent registry or committed reservations");
    first.Register("first-restarted", common::kProtocolVersion, true, "machine", "first");
    second.Register("second-restarted", common::kProtocolVersion, true, "machine", "second");
    Require(server->createRun("scheduler-probe", "restart-first", "") &&
            server->createRun("other-probe", "restart-second", ""), "restart runs failed");
    first.Send(request); first.Wait(common::NO_TASK);
    second.Send(request); second.Wait(common::NO_TASK);
    Require(control("release_clients", "token", {"empty", "machine"}).value("ok", false),
            "offline reservation recovery could not release clients");
    first.Wait(common::WAKE); second.Wait(common::WAKE);
    Require(!first.Request().task_id().empty() && !second.Request().task_id().empty(),
            "released plugin workers did not resume task execution");
    Require(control("release_clients", "token", {"empty", "machine"}).value("ok", false),
            "reservation release retry was not idempotent");
    Require(control("reserve_clients", "new-token", {"empty"}).value("http_status", 0) == 409,
            "persisted agent was online before a fresh heartbeat");
    Require(heartbeat("empty", "unconfigured").value("ok", false), "empty agent did not reconnect");
    Require(control("reserve_clients", "expiring", {"empty"}).value("ok", false), "pending reserve failed");
    std::this_thread::sleep_for(1050ms);
    Require(control("commit_reservation", "expiring", {"empty"}).value("http_status", 0) == 409,
            "expired pending reservation was committed");
    Require(control("reserve_clients", "replacement", {"empty"}).value("ok", false),
            "expired pending reservation blocked another deployment");
    server->stop();
}

void TestPluginRollout(const std::filesystem::path& bin) {
    Workspace workspace;
    auto& config = common::Config::instance();
    const auto config_file = workspace.root / "scheduler.json";
    std::unique_ptr<scheduler::Scheduler> server;
    std::string endpoint;
    for (int attempt = 0; attempt < 50; ++attempt) {
        const auto port = 30000 + (std::chrono::steady_clock::now().time_since_epoch().count() % 20000);
        endpoint = "tcp://127.0.0.1:" + std::to_string(port);
        nlohmann::json settings = {{"server", {{"router_endpoint", endpoint}, {"poll_timeout_ms", 5},
            {"client_timeout_ms", 0}, {"task_ack_timeout_ms", 10000}}},
            {"plugins", {{"probe", {{"enabled", true}, {"dll", (bin / "scheduler_probe.dll").string()},
                {"entered_file", (workspace.root / "entered").string()}}}}}};
        std::ofstream(config_file) << settings.dump();
        config.load(config_file.u8string());
        server = std::make_unique<scheduler::Scheduler>(config, config_file.u8string());
        try { server->start(); break; } catch (...) { server.reset(); }
    }
    Require(server != nullptr, "could not bind rollout scheduler test port");
    const std::string type = "scheduler-probe", target(64, 'a'), second_target(64, 'b');
    const auto command = [&](const std::string& action, const std::string& token = "rollout-one", nlohmann::json extra = nlohmann::json::object()) {
        extra["action"] = action; extra["plugin_type"] = type; extra["rollout_id"] = token;
        return server->managementCommand(extra);
    };
    const auto begin = [&](const std::string& token, const std::string& target_id, nlohmann::json extra = nlohmann::json::object()) {
        std::string bytes;
        Require(common::ReadCompleteFile(config_file, &bytes), "cannot read rollout config");
        extra["config_path"] = config_file.u8string(); extra["config_revision"] = common::Sha256Hex(bytes);
        extra["target_id"] = target_id; extra["target_patch"] = nlohmann::json::object();
        return command("begin_plugin_rollout", token, extra);
    };
    const auto heartbeat = [&](const std::string& revision, const std::string& state = "ready") {
        return server->managementCommand({{"action", "agent_heartbeat"}, {"client_id", "machine"},
            {"instance_id", "agent-instance"}, {"state", state}, {"plugin_keys", {"probe"}},
            {"active_revision", revision}, {"update_window_open", true}, {"capabilities", {"deployment_target_id_v1"}}});
    };
    const auto maintenance = [&](const std::string& action) {
        std::string bytes;
        Require(common::ReadCompleteFile(config_file, &bytes), "cannot read server maintenance configuration");
        return server->managementCommand({{"action", action}, {"plugin_type", type}, {"reservation_id", "server-dll-maintenance"},
            {"config_path", config_file.u8string()}, {"config_revision", common::Sha256Hex(bytes)}, {"committed", false}});
    };
    Peer old(endpoint, "machine/probe");
    old.Register("original", common::kProtocolVersion, true, "machine", "probe");
    Require(heartbeat("old").value("ok", false), "rollout agent heartbeat failed");
    const auto stopped = server->managementCommand({{"action", "run"}, {"plugin_type", type}, {"dataset_version", "stopped"}});
    Require(stopped.value("ok", false) && server->stopRun(type), "stopped rollout fixture failed");
    Require(begin("stopped-rollout", target).value("ok", false), "stopped batch prevented saving an update request");
    Require(!command("activate_plugin_rollout", "stopped-rollout").value("ok", false), "stopped batch was ignored");
    Require(!command("resume_rollout_run", "stopped-rollout", {{"run_id", stopped["run_id"]}}).value("ok", false),
        "unsupported plugin recovery was treated as a resumed batch");
    server->stop(); server.reset();
    config.load(config_file.u8string());
    server = std::make_unique<scheduler::Scheduler>(config, config_file.u8string()); server->start();
    const auto persisted_stop = command("get_plugin_rollout", "stopped-rollout");
    Require(!persisted_stop.value("ready_to_switch", true) && !persisted_stop["blockers"].empty() &&
        persisted_stop["blockers"][0]["status"] == "stopped", "restart forgot stopped rollout boundary");
    Require(!command("abandon_rollout_run", "stopped-rollout", {{"run_id", stopped["run_id"]}}).value("ok", false),
        "stopped batch was abandoned without a reason");
    Require(command("abandon_rollout_run", "stopped-rollout", {{"run_id", stopped["run_id"]}, {"reason", "explicitly abandon old work"}}).value("ok", false),
        "explicit stopped batch abandonment failed");
    Require(command("cancel_plugin_rollout", "stopped-rollout").value("ok", false), "cancel after explicit abandonment failed");
    old.Register("original-after-stop", common::kProtocolVersion, true, "machine", "probe");
    Require(heartbeat("old").value("ok", false), "rollout agent reconnect failed");
    const auto first = server->managementCommand({{"action", "run"}, {"plugin_type", type}, {"dataset_version", "a"}});
    const auto queued = server->managementCommand({{"action", "run"}, {"plugin_type", type}, {"dataset_version", "b"}});
    Require(first.value("ok", false) && queued.value("ok", false) && queued["status"] == "queued", "old rollout batches failed");
    const auto started = begin("rollout-one", target);
    Require(started.value("ok", false) && started["boundary"].size() == 2 && !started["ready_to_switch"].get<bool>(),
        "rollout did not capture active and queued batches");
    Require(begin("rollout-one", target).value("ok", false), "begin rollout was not idempotent");
    Require(!begin("competitor", target).value("ok", false), "a second rollout stole the plugin");
    Require(!maintenance("begin_plugin_maintenance").value("ok", false), "server DLL maintenance crossed a waiting rollout");
    Require(!server->createRun(type, "later", ""), "later CLI run crossed the rollout boundary");
    Require(!server->managementCommand({{"action", "reserve_clients"}, {"client_ids", {"machine"}},
        {"reservation_id", "too-early"}}).value("ok", false), "idle gap let another deployment reserve old-batch workers");
    const auto first_task = old.Request();
    Require(first_task.run_id() == first["run_id"], "rollout blocked or changed the old active batch");
    Require(!command("activate_plugin_rollout").value("ok", false), "rollout switched with a result in flight");
    old.Report(first_task, first_task.attempt_id());
    Require(old.Wait(common::TASK_REPORT_ACK).task_report_ack().accepted(), "old active report rejected");
    old.Wait(common::WAKE);
    const auto queued_task = old.Request();
    Require(queued_task.run_id() == queued["run_id"], "old queued batch failed to execute during waiting");
    old.Report(queued_task, queued_task.attempt_id());
    Require(old.Wait(common::TASK_REPORT_ACK).task_report_ack().accepted(), "old queued report rejected");
    for (int i = 0; i < 30 && !command("get_plugin_rollout").value("ready_to_switch", false); ++i)
        std::this_thread::sleep_for(20ms);
    Require(command("get_plugin_rollout").value("ready_to_switch", false), "completed boundary never became ready");
    Require(command("activate_plugin_rollout").value("ok", false), "empty DLL-only target did not enter switching");
    Require(!command("cancel_plugin_rollout").value("ok", false), "switching rollout was cancelled");
    Require(!command("finish_plugin_rollout").value("ok", false), "old registered worker was counted as new-target ready");
    Require(!server->createRun(type, "during-switch", ""), "new run started before any ready target worker");
    old.Register("new-config", common::kProtocolVersion, true, "machine", "probe", target);
    Require(heartbeat("new-whole-config", "updating").value("ok", false), "updating heartbeat failed");
    Require(!command("finish_plugin_rollout").value("ok", false), "worker registration preceded Agent commit but finished rollout");
    Require(heartbeat("new-whole-config").value("ok", false), "ready heartbeat failed");
    Require(command("finish_plugin_rollout").value("ok", false), "confirmed new target failed to finish rollout");
    Require(maintenance("begin_plugin_maintenance").value("ok", false), "completed idle target prevented independent server DLL maintenance");
    Require(!server->createRun(type, "during-server-maintenance", ""), "server maintenance failed to block new work");
    Require(maintenance("end_plugin_maintenance").value("ok", false), "server DLL maintenance could not release its original configuration");
    Require(command("get_plugin_rollout")["admission_target"] == target, "server DLL maintenance erased client target admission");
    Peer stale(endpoint, "late-old/probe");
    stale.Register("late-old", common::kProtocolVersion, true, "late-old", "probe");
    Require(server->createRun(type, "a", ""), "client DLL-only target change reused the old work fingerprint");
    common::Envelope request; request.set_type(common::REQUEST_TASK); request.mutable_request_task()->set_count(1);
    stale.Send(request); stale.Wait(common::NO_TASK);
    // A sibling plugin changes the whole-machine deployment revision. The
    // unchanged per-plugin resource target must retain admission.
    Require(heartbeat("sibling-plugin-new-revision").value("ok", false), "sibling revision heartbeat failed");
    old.Register("same-target-new-session", common::kProtocolVersion, true, "machine", "probe", target);
    const auto new_task = old.Request();
    old.Report(new_task, new_task.attempt_id());
    Require(old.Wait(common::TASK_REPORT_ACK).task_report_ack().accepted(), "new-target result rejected");
    for (int i = 0; i < 30 && server->activeRunCount(); ++i) std::this_thread::sleep_for(20ms);
    server->stop(); server.reset();
    // A separately maintained server DLL may expose a changed runtime type
    // under the same configuration key. It must not become an ungoverned plugin.
    std::string original_config;
    Require(common::ReadCompleteFile(config_file, &original_config), "cannot read original server type configuration");
    auto changed_type = nlohmann::json::parse(original_config);
    changed_type["plugins"]["probe"]["plugin_type"] = "changed-probe";
    std::ofstream(config_file) << changed_type.dump();
    config.load(config_file.u8string());
    server = std::make_unique<scheduler::Scheduler>(config, config_file.u8string()); server->start();
    {
        Peer changed_worker(endpoint, "changed-worker", "changed-probe");
        changed_worker.Register("changed-runtime", common::kProtocolVersion, true, "changed-machine", "probe");
        Require(server->createRun("changed-probe", "type-change", ""), "changed server type run fixture failed");
        changed_worker.Send(request); changed_worker.Wait(common::NO_TASK);
        Require(!server->managementStatus()["clients"][0]["rollout_eligible"].get<bool>(),
            "server type change discarded the original client target gate");
    }
    server->stop(); server.reset();
    std::ofstream(config_file) << original_config;
    config.load(config_file.u8string());
    server = std::make_unique<scheduler::Scheduler>(config, config_file.u8string()); server->start();
    Require(command("get_plugin_rollout")["admission_target"] == target, "restart lost admission target");
    stale.Register("late-old-restarted", common::kProtocolVersion, true, "late-old", "probe");
    old.Register("new-restarted", common::kProtocolVersion, true, "machine", "probe", target);
    Require(heartbeat("sibling-plugin-new-revision").value("ok", false), "restart heartbeat failed");
    Require(begin("rollout-two", second_target).value("ok", false), "completed rollout blocked subsequent update");
    Require(command("cancel_plugin_rollout", "rollout-two").value("ok", false), "waiting rollout cancellation failed");
    Require(command("get_plugin_rollout", "rollout-two")["admission_target"] == target, "cancel erased prior target admission");
    Require(begin("rollout-three", second_target).value("ok", false) &&
        command("activate_plugin_rollout", "rollout-three").value("ok", false), "second switching target failed");
    Require(begin("repair", std::string(64, 'c'), {{"replaces_rollout_id", "rollout-three"}}).value("ok", false),
        "failed switching target could not be atomically replaced");
    Require(command("get_plugin_rollout", "rollout-three")["phase"] == "superseded", "replaced rollout history was lost");
    Require(!command("cancel_plugin_rollout", "repair").value("ok", false), "replacement reopened old target admission");
    server->stop();
}
}
int main(int argc,char** argv) {
    try { const auto bin = std::filesystem::absolute(argv[0]).parent_path(); Test(bin); TestParallelPlugins(bin); TestClientControl(bin); TestOnlineDeployment(bin); TestPluginRollout(bin);
        std::cout<<"PASS: session takeover, durable report identity, command barrier, independent heartbeat, business outcome deduplication, parallel plugin queues, physical client targeting, empty agent registry and durable deployment reservations\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
