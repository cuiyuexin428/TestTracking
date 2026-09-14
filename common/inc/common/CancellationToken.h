#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace common {

namespace detail {
struct CancellationState {
    std::atomic<bool> requested{false};
    std::mutex mutex;
    std::condition_variable changed;
};
}  // namespace detail

// A copyable, read-only view of one execution generation's cancellation.
// A default token never cancels. Copies remain valid after the source dies.
class CancellationToken {
public:
    CancellationToken() = default;

    bool isCancellationRequested() const {
        return state_ && state_->requested.load(std::memory_order_acquire);
    }

    // Returns true on cancellation, false when the duration elapsed.
    bool waitFor(std::chrono::milliseconds duration) const {
        if (!state_) {
            std::this_thread::sleep_for(duration);
            return false;
        }
        std::unique_lock<std::mutex> lock(state_->mutex);
        return state_->changed.wait_for(lock, duration, [this] {
            return state_->requested.load(std::memory_order_acquire);
        });
    }

private:
    explicit CancellationToken(std::shared_ptr<detail::CancellationState> state)
        : state_(std::move(state)) {}
    std::shared_ptr<detail::CancellationState> state_;
    friend class CancellationSource;
};

class CancellationSource {
public:
    CancellationSource() = default;
    void reset() { state_ = std::make_shared<detail::CancellationState>(); }
    CancellationToken token() const { return CancellationToken(state_); }

    void requestCancellation() const {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->requested.store(true, std::memory_order_release);
        }
        state_->changed.notify_all();
    }

private:
    std::shared_ptr<detail::CancellationState> state_ =
        std::make_shared<detail::CancellationState>();
};

}  // namespace common
