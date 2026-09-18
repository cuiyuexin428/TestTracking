#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace scheduler {
class Scheduler;

// Local bridge used by the LAN-facing result service. No runtime dependency
// beyond the platform socket library is needed in offline packages.
class ManagementServer {
 public:
    explicit ManagementServer(Scheduler& scheduler) : scheduler_(scheduler) {}
    ~ManagementServer();
    void start(const std::string& endpoint);
    void stop();

 private:
    void Run();
    void Handle(std::intptr_t socket);
    Scheduler& scheduler_;
    std::atomic<bool> running_{false};
    std::intptr_t listener_ = -1;
    std::thread thread_;
    bool socket_runtime_ = false;
};
}  // namespace scheduler
