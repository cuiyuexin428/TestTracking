#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <utility>

namespace common {

// Minimal blocking queue. This is a placeholder skeleton; timeout-aware
// pop and close semantics will be added when the component is formally
// implemented.
template <typename T>
class BlockingQueue {
public:
    // Accepts both lvalues (copied) and rvalues (moved).
    void push(T item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(item));
        }
        cv_.notify_one();
    }

    // Blocks until an item is available.
    T pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty(); });
        T item = std::move(queue_.front());
        queue_.pop();
        return item;
    }

    // Blocks until an item is available or the timeout expires. Returns false
    // on timeout without modifying item.
    template <typename Rep, typename Period>
    bool pop(T& item, const std::chrono::duration<Rep, Period>& timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [this] { return !queue_.empty(); })) {
            return false;
        }
        item = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    // Wakes all waiters, e.g. to unblock workers on shutdown.
    void notifyAll() {
        cv_.notify_all();
    }

    // Number of queued items. Safe to call from any thread.
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    // Whether the queue is empty. Safe to call from any thread.
    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    // Drops all queued items. Safe to call from any thread.
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!queue_.empty()) {
            queue_.pop();
        }
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<T> queue_;
};

}  // namespace common
