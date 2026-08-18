#include "common/EventManager.h"

#include <cassert>
#include <chrono>
#include <stdexcept>

namespace common {

	namespace {

		const auto kPopTimeout = std::chrono::milliseconds(100);

	}  // namespace

	EventManager::EventManager(int thread_num) {
		if (thread_num < 1) {
			throw std::invalid_argument("EventManager: thread_num must be >= 1");
		}

		thread_group.reserve(thread_num);
		for (int i = 0; i < thread_num; ++i) {
			thread_group.emplace_back([this] {
				while (!stop_.load(std::memory_order_relaxed)) {
					std::unique_ptr<Event> event;
					if (queue_.pop(event, kPopTimeout)) {
						event->execute();
					}
				}
				});
		}
	}

	EventManager::~EventManager() {
		stop_.store(true, std::memory_order_relaxed);
		// Drop queued events before waking workers so nothing queued runs after stop.
		{
			std::lock_guard<std::mutex> lock(push_mutex_);
			queue_.clear();
		}
		// Wake blocked workers immediately so they notice stop_ without waiting
		// for the pop timeout. Workers busy executing finish, then exit.
		queue_.notifyAll();
		for (std::thread& t : thread_group) {
			if (t.joinable()) {
				t.join();
			}
		}
	}

	void EventManager::push(Event* event) {
		// The queue owns events as unique_ptr; ownership transfers to the manager.
		// Fail fast in debug builds; keep the guard for release builds where
		// assert() is compiled out.
		assert(event != nullptr);
		if (event == nullptr) {
			return;
		}
		{
			std::lock_guard<std::mutex> lock(push_mutex_);
			if (stop_.load(std::memory_order_relaxed)) {
				delete event;
				return;
			}
			queue_.push(std::unique_ptr<Event>(event));
		}
	}

}  // namespace common
