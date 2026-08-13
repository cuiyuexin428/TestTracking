// Demo client: a ZmqDealer that sends stdin lines to the server and prints replies.
#include "common/zmq_helper.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>

namespace {

const char* kDefaultEndpoint = "tcp://127.0.0.1:5555";

}  // namespace

int main(int argc, char* argv[]) {
    const std::string endpoint = (argc > 1) ? argv[1] : kDefaultEndpoint;

    try {
        common::ZmqDealer dealer(endpoint);
        const char* identity = "demo-client";
        dealer.setOption(ZMQ_IDENTITY, identity, std::strlen(identity));
        const int recvTimeoutMs = 5000;
        dealer.setOption(ZMQ_RCVTIMEO, &recvTimeoutMs, sizeof(recvTimeoutMs));
        dealer.connect();
        std::printf("client: connected to %s, type a line and press Enter ('quit' to exit)\n",
                    endpoint.c_str());

        std::string line;
        while (std::getline(std::cin, line)) {
            if (line == "quit" || line == "exit") {
                // Tell the server to stop, then leave without waiting for a reply.
                dealer.sendMsg("quit");
                break;
            }
            dealer.sendMsg(line);
            try {
                const std::string reply = dealer.recvMsg();
                std::printf("client: %s\n", reply.c_str());
            } catch (const std::exception& e) {
                std::fprintf(stderr, "client: recv failed: %s\n", e.what());
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "client: %s\n", e.what());
        return 1;
    }
    return 0;
}
