// Smoke test: ZmqDealer <-> ZmqRouter round trip over loopback TCP.
#include "common/zmq_helper.h"

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace {

const char* kHost = "tcp://127.0.0.1:";

std::string Endpoint(int port) {
    return std::string(kHost) + std::to_string(port);
}

int Run() {
    const int kStartPort = 28510;
    const int kReceiveTimeoutMs = 5000;

    for (int port = kStartPort; port < kStartPort + 10; ++port) {
        try {
            common::ZmqRouter router(Endpoint(port));
            router.setOption(ZMQ_RCVTIMEO, &kReceiveTimeoutMs, sizeof(kReceiveTimeoutMs));
            router.bind();

            common::ZmqDealer dealer(Endpoint(port));
            dealer.setOption(ZMQ_RCVTIMEO, &kReceiveTimeoutMs, sizeof(kReceiveTimeoutMs));
            dealer.connect();

            const std::string request = "hello from dealer";
            dealer.sendMsg(request);

            // ROUTER receives the dealer identity frame first, then the payload.
            std::vector<std::string> parts = router.recvMsgParts();
            if (parts.size() != 2) {
                std::fprintf(stderr, "FAIL: expected 2 parts, got %zu\n", parts.size());
                return 1;
            }
            if (parts[1] != request) {
                std::fprintf(stderr, "FAIL: request mismatch: %s\n", parts[1].c_str());
                return 1;
            }

            // Reply must echo the identity back as the first frame.
            const std::string reply = "pong from router";
            router.sendMsgParts({parts[0], reply});

            if (dealer.recvMsg() != reply) {
                std::fprintf(stderr, "FAIL: reply mismatch\n");
                return 1;
            }

            std::printf("PASS: ZmqDealer/ZmqRouter round trip on port %d\n", port);
            return 0;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "port %d failed: %s\n", port, e.what());
        }
    }

    std::fprintf(stderr, "FAIL: no free port found\n");
    return 1;
}

}  // namespace

int main() {
    return Run();
}
