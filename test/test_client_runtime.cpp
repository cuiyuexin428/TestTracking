#include "client/TaskExecutor.h"
#include "common/BlockingQueue.h"
#include "common/EventManager.h"
#include "common/ProcessRunner.h"
#include "common/zmq_helper.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

namespace {
using namespace std::chrono_literals;

void Require(bool passed, const char* message) {
    if (!passed) throw std::runtime_error(message);
}

template<class Predicate>
bool Until(Predicate predicate, std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}

class Action : public common::Event {
public:
    explicit Action(std::function<void()> action) : action_(std::move(action)) {}
    void execute() override { action_(); }
private:
    std::function<void()> action_;
};

class Worker : public common::WorkerPlugin {
public:
    std::atomic<bool> entered{false};
    std::atomic<bool> delay{true};
    std::string pluginType() const override { return "runtime-test"; }
    bool init(const std::string&) override { return true; }
    bool collectRegistrationInfo(std::string*) override { return true; }
    common::RegisterOutcome handleValidationReply(const std::string&) override {
        return common::RegisterOutcome::kReady;
    }
    bool execute(const common::Task&, common::TaskResult&) override { return false; }
    bool execute(const common::Task&, common::TaskResult& result,
                 const common::CancellationToken& token) override {
        entered = true;
        if (delay) token.waitFor(10s);
        result.Clear(); // Identity must be supplied by the executor.
        return !token.isCancellationRequested();
    }
};

void TestQueueAndExecutor() {
    common::BlockingQueue<int> queue;
    auto waiter = std::async(std::launch::async, [&] {
        int item = 42;
        return queue.pop(item, 24h);
    });
    queue.close();
    Require(waiter.wait_for(200ms) == std::future_status::ready && !waiter.get(),
            "closed queue did not wake waiter");
    Require(!queue.push(1), "closed queue accepted producer");

    common::CancellationSource cancel;
    common::EventManager events(1);
    std::atomic<bool> entered{false};
    std::atomic<int> queued{0};
    events.push(new Action([&] { entered = true; cancel.token().waitFor(10s); }));
    Require(Until([&] { return entered.load(); }), "event did not begin");
    events.push(new Action([&] { ++queued; }));
    const auto start = std::chrono::steady_clock::now();
    events.requestStop();
    Require(std::chrono::steady_clock::now() - start < 100ms,
            "requestStop blocked on running event");
    Require(!events.isStopped(), "worker incorrectly declared stopped before action completed");
    cancel.requestCancellation();
    Require(Until([&] { return events.isStopped(); }), "cancelled event failed to drain");
    events.join();
    Require(queued == 0, "queued event executed after stop");

    Worker plugin;
    client::TaskExecutor executor;
    common::BlockingQueue<common::TaskResult> results;
    const auto callback = [&](const common::TaskResult& result) { results.push(result); };
    executor.start(&plugin, callback, 1);
    common::Task task;
    task.set_task_id("task");
    task.set_run_id("run");
    task.set_attempt_id("first");
    executor.push(task);
    Require(Until([&] { return plugin.entered.load(); }), "worker did not execute");
    executor.requestStop();
    Require(Until([&] { return executor.isStopped(); }), "executor cancellation failed");
    executor.stop();
    Require(results.empty(), "cancelled execution delivered a completion");
    plugin.delay = false;
    task.set_attempt_id("second");
    executor.start(&plugin, callback, 1);
    executor.push(task);
    common::TaskResult result;
    Require(results.pop(result, 2s), "restarted executor did not complete");
    Require(result.task_id() == "task" && result.run_id() == "run" &&
            result.attempt_id() == "second", "execution identity not preserved");
    executor.stop();
}

void TestBackpressure() {
    common::ZmqDealer dealer("tcp://127.0.0.1:1");
    const int immediate = 1;
    const int high_water = 1;
    dealer.setOption(ZMQ_IMMEDIATE, &immediate, sizeof(immediate));
    dealer.setOption(ZMQ_SNDHWM, &high_water, sizeof(high_water));
    // Deliberately leave this socket disconnected; the test touches no service.
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i)
        Require(!dealer.trySendMsg("unreachable"), "unconnected immediate socket accepted message");
    Require(std::chrono::steady_clock::now() - start < 1s, "send backpressure blocked");
}

int ChildMode(int argc, char** argv) {
    const std::string mode = argv[1];
    if (mode == "--hold") {
        if (argc > 2) {
            std::ofstream marker(argv[2]);
#ifdef _WIN32
            marker << GetCurrentProcessId();
#else
            marker << getpid();
#endif
        }
        std::this_thread::sleep_for(30s);
        return 0;
    }
    if (mode == "--tree") {
        common::ProcessOptions child;
        child.executable = std::filesystem::absolute(argv[0]).string();
        child.arguments = {"--hold", argv[2]};
        return common::ProcessRunner::run(child).exit_code;
    }
    if (mode == "--record") {
        std::ofstream output(argv[2], std::ios::binary);
        output << std::filesystem::current_path().string() << '\n';
        for (int i = 3; i < argc; ++i) output << argv[i] << '\n';
        return output ? 17 : 18;
    }
    return 99;
}

void TestProcesses(const std::string& executable, const std::filesystem::path& directory) {
    common::ProcessOptions options;
    options.executable = executable;
    options.working_directory = directory.string();
    const auto recorded = directory / "argument results.txt";
    options.arguments = {"--record", recorded.string(), "", "hello world", "a\"b", "C:\\space dir\\", "a&b"};
    const auto original_directory = std::filesystem::current_path();
    const auto result = common::ProcessRunner::run(options);
    Require(result.started && result.exit_code == 17 && !result.cancelled, "child argv helper failed");
    Require(std::filesystem::current_path() == original_directory, "runner changed process cwd");
    std::ifstream output(recorded);
    std::string line;
    std::getline(output, line);
    Require(std::filesystem::path(line) == directory, "child cwd incorrect");
    for (size_t index = 2; index < options.arguments.size(); ++index) {
        std::getline(output, line);
        Require(line == options.arguments[index], "literal argument was not preserved");
    }

    options.executable = (directory / "no-such-program.exe").string();
    const auto failed = common::ProcessRunner::run(options);
    Require(!failed.started && !failed.error.empty(), "launch failure was not reported");
    options.executable = executable;
    options.arguments = {"--hold"};
    options.timeout = 100ms;
    const auto timeout = common::ProcessRunner::run(options);
    Require(timeout.started && timeout.timed_out && !timeout.cancelled, "timeout did not kill process");
    common::CancellationSource pre_cancelled;
    pre_cancelled.requestCancellation();
    const auto skipped = common::ProcessRunner::run(options, pre_cancelled.token());
    Require(!skipped.started && skipped.cancelled, "cancelled operation launched child");

    const auto marker = directory / "descendant.pid";
    options.arguments = {"--tree", marker.string()};
    options.timeout = 10s;
    common::CancellationSource cancellation;
    auto running = std::async(std::launch::async, [&] {
        return common::ProcessRunner::run(options, cancellation.token());
    });
    unsigned long child_id = 0;
    const bool child_started = Until([&] {
        std::ifstream input(marker);
        return bool(input >> child_id) && child_id != 0;
    });
#ifdef _WIN32
    HANDLE child = child_started ? OpenProcess(SYNCHRONIZE, FALSE, child_id) : nullptr;
#endif
    cancellation.requestCancellation();
    Require(child_started, "nested process did not start");
    Require(running.wait_for(3s) == std::future_status::ready, "process cancellation did not finish");
    Require(running.get().cancelled, "process cancellation missing from outcome");
#ifdef _WIN32
    Require(child != nullptr, "could not observe descendant process");
    const auto wait = WaitForSingleObject(child, 3000);
    CloseHandle(child);
    Require(wait == WAIT_OBJECT_0, "descendant survived Job Object cancellation");
#else
    Require(Until([&] { return kill(static_cast<pid_t>(child_id), 0) != 0; }),
            "descendant survived process group cancellation");
#endif
}
}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) return ChildMode(argc, argv);
    try {
        const auto directory = std::filesystem::temp_directory_path() /
            ("testtracking-runtime-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory);
        TestQueueAndExecutor();
        TestBackpressure();
        TestProcesses(std::filesystem::absolute(argv[0]).string(), directory);
        std::cout << "PASS: cancellation, process tree, timeout, argv/cwd, executor and backpressure\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
