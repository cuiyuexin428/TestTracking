#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <mutex>
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
    ~EventManager();

    void push(Event* event);

private:
    EventQueue queue_;
    std::vector<std::thread> thread_group;
    std::atomic<bool> stop_{false};
    std::mutex push_mutex_;
};

}  // namespace common
