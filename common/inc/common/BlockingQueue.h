#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <deque>
#include <utility>
#include <stdexcept>

namespace common {

// Thread-safe blocking queue with timeout-aware pop, pushFront and clear.
// pop blocks until an item is available or the timeout expires; pushFront
// re-queues an item at the head for retry.
template <typename T>
class BlockingQueue {
public:
    // Accepts both lvalues (copied) and rvalues (moved).
    bool push(T item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) return false;
            queue_.push_back(std::move(item));
        }
        cv_.notify_one();
        return true;
    }

    // Pushes to the front; used to retry a head message without reordering
    // the rest of the queue.
    bool pushFront(T item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) return false;
            queue_.push_front(std::move(item));
        }
        cv_.notify_one();
        return true;
    }

    // Blocks until an item is available.
    T pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) throw std::runtime_error("BlockingQueue: closed");
        T item = std::move(queue_.front());
        queue_.pop_front();
        return item;
    }

    // Blocks until an item is available or the timeout expires. Returns false
    // on timeout without modifying item.
    template <typename Rep, typename Period>
    bool pop(T& item, const std::chrono::duration<Rep, Period>& timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [this] { return closed_ || !queue_.empty(); }) ||
            queue_.empty()) {
            return false;
        }
        item = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    // Wakes waiters to recheck their predicate; use close() for shutdown.
    void notifyAll() {
        cv_.notify_all();
    }

    // Reject future pushes and release blocked consumers. By default the
    // existing queue drains; discard=true drops pending work immediately.
    void close(bool discard = false) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
            if (discard) queue_.clear();
        }
        cv_.notify_all();
    }

    bool isClosed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
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
            queue_.pop_front();
        }
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<T> queue_;
    bool closed_ = false;
};

}  // namespace common
