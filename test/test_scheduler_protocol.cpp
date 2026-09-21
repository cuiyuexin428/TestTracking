#include "scheduler/Scheduler.h"
#include "common/Protocol.h"
#include "common/zmq_helper.h"
#include <json.hpp>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <utility>

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
                              const std::string& plugin_key = {}) {
        common::Envelope request; request.set_type(common::REGISTER);
        request.mutable_register_()->set_registration_id(nonce);
        request.mutable_register_()->set_protocol_version(protocol);
        request.mutable_register_()->set_physical_client_id(physical_client);
        request.mutable_register_()->set_plugin_key(plugin_key);
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
    void Heartbeat() {
        common::Envelope message; message.set_type(common::HEARTBEAT);
        message.mutable_heartbeat(); Send(message);
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
}
int main(int argc,char** argv) {
    try { const auto bin = std::filesystem::absolute(argv[0]).parent_path(); Test(bin); TestParallelPlugins(bin);
        std::cout<<"PASS: session takeover, durable report identity, command barrier, independent heartbeat, business outcome deduplication, parallel plugin queues and physical client targeting\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
