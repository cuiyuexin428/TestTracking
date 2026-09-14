#include "common/EventManager.h"
#include "common/Logger.h"

#include <chrono>
#include <stdexcept>

namespace common {

EventManager::EventManager(int thread_num) {
    if (thread_num < 1) {
        throw std::invalid_argument("EventManager: thread_num must be >= 1");
    }
    thread_group.reserve(thread_num);
    try {
        for (int i = 0; i < thread_num; ++i) {
            running_workers_.fetch_add(1);
            try {
                thread_group.emplace_back([this] {
                    std::unique_ptr<Event> event;
                    while (queue_.pop(event, std::chrono::hours(24))) {
                        if (stop_.load(std::memory_order_acquire)) break;
                        try {
                            event->execute();
                        } catch (const std::exception& error) {
                            LOG_ERROR("worker event failed: " << error.what());
                        } catch (...) {
                            LOG_ERROR("worker event failed: unknown exception");
                        }
                        event.reset();
                    }
                    event.reset();
                    running_workers_.fetch_sub(1, std::memory_order_release);
                });
            } catch (...) {
                running_workers_.fetch_sub(1);
                throw;
            }
        }
    } catch (...) {
        requestStop();
        join();
        throw;
    }
}

EventManager::~EventManager() {
    requestStop();
    join();
}

bool EventManager::push(Event* event) {
    return push(std::unique_ptr<Event>(event));
}

bool EventManager::push(std::unique_ptr<Event> event) {
    return event && queue_.push(std::move(event));
}

void EventManager::requestStop() {
    stop_.store(true, std::memory_order_release);
    queue_.close(true);
}

bool EventManager::isStopped() const {
    return running_workers_.load(std::memory_order_acquire) == 0;
}

void EventManager::join() {
    for (auto& worker : thread_group) {
        if (worker.joinable()) worker.join();
    }
}

}  // namespace common
