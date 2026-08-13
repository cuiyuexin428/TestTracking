// Demo server: a ZmqRouter that echoes every client message.
#include "common/zmq_helper.h"

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace {

const char* kDefaultEndpoint = "tcp://*:5555";

}  // namespace

int main(int argc, char* argv[]) {
    const std::string endpoint = (argc > 1) ? argv[1] : kDefaultEndpoint;

    try {
        common::ZmqRouter router(endpoint);
        router.bind();
        std::printf("server: bound to %s, waiting for messages ('quit' stops the server)\n",
                    endpoint.c_str());

        while (true) {
            std::vector<std::string> parts = router.recvMsgParts();
            if (parts.size() < 2) {
                std::fprintf(stderr, "server: malformed message (%zu parts), ignored\n", parts.size());
                continue;
            }
            const std::string& identity = parts[0];
            const std::string& msg = parts[1];

            std::printf("server: received '%s' from %s\n", msg.c_str(), identity.c_str());

            if (msg == "quit") {
                std::printf("server: quitting\n");
                break;
            }

            const std::string reply = "echo: " + msg;
            router.sendMsgParts({identity, reply});
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "server: %s\n", e.what());
        return 1;
    }
    return 0;
}
