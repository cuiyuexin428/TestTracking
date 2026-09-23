#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "common/BlockingQueue.h"
#include "common/Event.h"

namespace common {

using EventQueue = BlockingQueue<std::unique_ptr<Event>>;

// Dispatches events pushed by producers to a pool of worker threads. Each
// worker pops an event, executes it, and the unique_ptr releases the event
// when the action finishes.
class EventManager {
public:
    explicit EventManager(int thread_num);
    EventManager(int thread_num, std::chrono::milliseconds idle_wait);
    ~EventManager();

    // Ownership always transfers, including when a closed queue rejects work.
    bool push(Event* event);
    bool push(std::unique_ptr<Event> event);
    // Discard queued work and wake waiters. Does not wait for running actions.
    void requestStop();
    bool isStopped() const;
    // Call after requestStop; only from the owner, never from a worker action.
    void join();

private:
    EventQueue queue_;
    std::vector<std::thread> thread_group;
    std::atomic<bool> stop_{false};
    std::atomic<int> running_workers_{0};
};

}  // namespace common
