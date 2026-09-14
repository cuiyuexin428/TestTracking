#include "scheduler/Server.h"

#include <algorithm>
#include <deque>
#include <unordered_map>

#include "common/Logger.h"

namespace scheduler {
Server::Server(const std::string& endpoint, const std::chrono::milliseconds& poll_timeout)
    : endpoint_(endpoint), poll_timeout_(poll_timeout) {
}
Server::~Server() {
    stop();
}
void Server::start() {
    std::promise<void> ready;
    auto result = ready.get_future();
    running_.store(true);
    thread_ = std::thread(&Server::RunLoop, this, std::move(ready));
    result.get();
}
void Server::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}
void Server::sendTo(const std::string& identity, const common::Envelope& envelope) {
    if (outgoing_.size() < 8192)
        outgoing_.push({identity, envelope});
    else
        LOG_WARN("server: outgoing queue full; protocol retry required");
}
bool Server::tryRecv(IncomingMessage& out, const std::chrono::milliseconds& timeout) {
    return incoming_.pop(out, timeout);
}
void Server::RunLoop(std::promise<void> ready) {
    bool initialized = false;
    try {
        // Creation, binding, I/O and destruction all have the same owner.
        common::ZmqRouter router(endpoint_);
        const int one = 1;
        router.setOption(ZMQ_ROUTER_HANDOVER, &one, sizeof(one));
        router.setOption(ZMQ_ROUTER_MANDATORY, &one, sizeof(one));
        router.bind();
        ready.set_value();
        initialized = true;
        struct Pending {
            std::string bytes;
            std::chrono::steady_clock::time_point queued;
        };
        std::unordered_map<std::string, std::deque<Pending>> pending;
        while (running_.load()) {
            if (router.waitReadable(std::min(poll_timeout_, std::chrono::milliseconds(10)))) {
                for (int i = 0; i < 128; ++i) {
                    const auto parts = router.recvMsgParts();
                    IncomingMessage msg;
                    if (parts.size() == 2 && msg.envelope.ParseFromString(parts[1])) {
                        msg.identity = parts[0];
                        msg.peer_address = router.lastPeerAddress();
                        msg.received_at = std::chrono::steady_clock::now();
                        if (msg.envelope.type() == common::HEARTBEAT &&
                            msg.envelope.has_heartbeat()) {
                            if (heartbeat_handler_) heartbeat_handler_(msg);
                        } else if (incoming_.size() < 8192)
                            incoming_.push(std::move(msg));
                    }
                    if (!router.waitReadable(std::chrono::milliseconds(0))) break;
                }
            }
            OutgoingMessage out;
            for (int i = 0; i < 256 && outgoing_.pop(out, std::chrono::milliseconds(0)); ++i) {
                // Bound both peer count and per-peer backlog.
                if (!pending.count(out.identity) && pending.size() >= 4096) continue;
                auto& queue = pending[out.identity];
                if (queue.size() < 256)
                    queue.push_back(
                        {out.envelope.SerializeAsString(), std::chrono::steady_clock::now()});
            }
            // Preserve per-client order without head-of-line blocking others.
            for (auto it = pending.begin(); it != pending.end();) {
                auto& queue = it->second;
                for (int n = 0; n < 32 && !queue.empty(); ++n) {
                    const bool expired = std::chrono::steady_clock::now() - queue.front().queued >
                                         std::chrono::seconds(10);
                    if (!expired && !router.trySendMsgParts({it->first, queue.front().bytes}))
                        break;
                    queue.pop_front();
                }
                if (queue.empty())
                    it = pending.erase(it);
                else
                    ++it;
            }
        }
    } catch (...) {
        running_.store(false);
        if (!initialized)
            ready.set_exception(std::current_exception());
        else
            LOG_ERROR("server: network loop failed");
    }
}
}  // namespace scheduler
