#include "scheduler/Server.h"

#include "common/Logger.h"

#include <exception>
#include <utility>

namespace scheduler {

Server::Server(const std::string& endpoint, const std::chrono::milliseconds& poll_timeout)
    : router_(endpoint), poll_timeout_(poll_timeout) {
}

Server::~Server() {
    stop();
}

void Server::start() {
    router_.bind();
    running_.store(true);
    thread_ = std::thread(&Server::RunLoop, this);
}

void Server::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

void Server::sendTo(const std::string& identity, const common::Envelope& envelope) {
    outgoing_.push({identity, envelope});
}

bool Server::tryRecv(IncomingMessage& out, const std::chrono::milliseconds& timeout) {
    return incoming_.pop(out, timeout);
}

void Server::RunLoop() {
    OutgoingMessage out;
    while (running_.load(std::memory_order_relaxed)) {
        try {
            if (router_.waitReadable(poll_timeout_)) {
                std::vector<std::string> parts = router_.recvMsgParts();
                if (parts.size() >= 2) {
                    common::Envelope env;
                    if (env.ParseFromString(parts[1])) {
                        incoming_.push({parts[0], std::move(env)});
                    } else {
                        LOG_WARN("server: dropped unparseable message from " << parts[0]);
                    }
                } else {
                    LOG_WARN("server: dropped malformed message (" << parts.size()
                                                                   << " parts) from "
                                                                   << (parts.empty()
                                                                           ? std::string("unknown")
                                                                           : parts[0]));
                }
            }
            if (outgoing_.pop(out, std::chrono::milliseconds(0))) {
                std::string bytes;
                if (!out.envelope.SerializeToString(&bytes)) {
                    LOG_ERROR("server: failed to serialize envelope");
                    continue;
                }
                if (router_.waitWritable(std::chrono::milliseconds(0))) {
                    router_.sendMsgParts({out.identity, bytes});
                } else {
                    // Send queue full (HWM): keep this message at the head so
                    // ordering is preserved and it is retried next round.
                    outgoing_.pushFront(std::move(out));
                    break;
                }
            }
        } catch (const std::exception& e) {
            // A zmq failure means the network layer is broken; there is no
            // point keeping the scheduler alive.
            LOG_ERROR("server: fatal zmq error: " << e.what());
            std::terminate();
        } catch (...) {
            LOG_ERROR("server: fatal zmq unknown error");
            std::terminate();
        }
    }
}

}  // namespace scheduler
