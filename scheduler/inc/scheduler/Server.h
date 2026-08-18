#pragma once

#include "common/BlockingQueue.h"
#include "common/zmq_helper.h"
#include "communication.pb.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace scheduler {

// A message received from a client: the ROUTER identity frame plus the
// parsed Envelope.
struct IncomingMessage {
    std::string identity;
    common::Envelope envelope;
    std::string peer_address;  // server-observed "ip:port" of the client
};

// A message waiting to be sent to a client.
struct OutgoingMessage {
    std::string identity;
    common::Envelope envelope;
};

// Owns the scheduler's ROUTER socket and runs the poll loop. The poll thread
// is the only thread that touches the socket: it receives messages into an
// incoming queue and drains the outgoing queue for sends.
class Server {
public:
    Server(const std::string& endpoint, const std::chrono::milliseconds& poll_timeout);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void start();
    void stop();

    // Thread-safe: queue a message for a client.
    void sendTo(const std::string& identity, const common::Envelope& envelope);

    // Blocks up to timeout for a received message; returns false on timeout.
    bool tryRecv(IncomingMessage& out, const std::chrono::milliseconds& timeout);

private:
    void RunLoop();

    common::ZmqRouter router_;
    std::chrono::milliseconds poll_timeout_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    common::BlockingQueue<IncomingMessage> incoming_;
    common::BlockingQueue<OutgoingMessage> outgoing_;
};

}  // namespace scheduler
