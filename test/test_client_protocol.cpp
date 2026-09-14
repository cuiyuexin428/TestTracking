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
}  // namespace

int main(int argc, char** argv) {
    try {
        const auto binary_directory = std::filesystem::absolute(argv[0]).parent_path();
        TestClient(binary_directory);
        TestInitialRegistrationRetryLimit(binary_directory);
        std::cout << "PASS: report retries, stale ACK, STOP heartbeat, re-registration, protocol "
                     "rejection and registration retry limit\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
