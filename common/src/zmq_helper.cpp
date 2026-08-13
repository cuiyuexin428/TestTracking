#include "common/zmq_helper.h"

#include <stdexcept>

namespace common {

namespace {

void Check(int rc, const char* what) {
    if (rc == -1) {
        throw std::runtime_error(std::string(what) + ": " + zmq_strerror(zmq_errno()));
    }
}

}  // namespace

template <int SocketType>
ZmqBase<SocketType>::ZmqBase(const std::string& ip_port) : ip_port_(ip_port) {
    context_ = zmq_ctx_new();
    if (context_ == nullptr) {
        throw std::runtime_error("zmq_ctx_new failed");
    }
    socket_ = zmq_socket(context_, SocketType);
    if (socket_ == nullptr) {
        zmq_ctx_term(context_);
        context_ = nullptr;
        throw std::runtime_error("zmq_socket failed");
    }
    // Drop queued messages immediately on close; otherwise zmq_close() and
    // zmq_ctx_term() may block forever on pending sends (default linger is -1).
    const int linger = 0;
    Check(zmq_setsockopt(socket_, ZMQ_LINGER, &linger, sizeof(linger)),
          "zmq_setsockopt(ZMQ_LINGER)");
}

template <int SocketType>
ZmqBase<SocketType>::~ZmqBase() {
    if (socket_ != nullptr) {
        zmq_close(socket_);
        socket_ = nullptr;
    }
    if (context_ != nullptr) {
        zmq_ctx_term(context_);
        context_ = nullptr;
    }
}

template <int SocketType>
void ZmqBase<SocketType>::bind() {
    Check(zmq_bind(socket_, ip_port_.c_str()), "zmq_bind");
}

template <int SocketType>
void ZmqBase<SocketType>::connect() {
    Check(zmq_connect(socket_, ip_port_.c_str()), "zmq_connect");
}

template <int SocketType>
bool ZmqBase<SocketType>::waitReadable(const std::chrono::milliseconds& timeout) {
    zmq_pollitem_t items[] = {{socket_, 0, ZMQ_POLLIN, 0}};
    const int rc = zmq_poll(items, 1, static_cast<int>(timeout.count()));
    if (rc < 0) {
        throw std::runtime_error(std::string("zmq_poll: ") + zmq_strerror(zmq_errno()));
    }
    return (items[0].revents & ZMQ_POLLIN) != 0;
}

template <int SocketType>
bool ZmqBase<SocketType>::waitWritable(const std::chrono::milliseconds& timeout) {
    zmq_pollitem_t items[] = {{socket_, 0, ZMQ_POLLOUT, 0}};
    const int rc = zmq_poll(items, 1, static_cast<int>(timeout.count()));
    if (rc < 0) {
        throw std::runtime_error(std::string("zmq_poll: ") + zmq_strerror(zmq_errno()));
    }
    return (items[0].revents & ZMQ_POLLOUT) != 0;
}

template <int SocketType>
std::string ZmqBase<SocketType>::recvMsg() {
    zmq_msg_t msg;
    Check(zmq_msg_init(&msg), "zmq_msg_init");

    const int rc = zmq_msg_recv(&msg, socket_, 0);
    if (rc == -1) {
        zmq_msg_close(&msg);
        throw std::runtime_error(std::string("zmq_msg_recv: ") + zmq_strerror(zmq_errno()));
    }

    std::string out(static_cast<const char*>(zmq_msg_data(&msg)), zmq_msg_size(&msg));
    if (zmq_msg_more(&msg) != 0) {
        zmq_msg_close(&msg);
        throw std::runtime_error("recvMsg(): message has more frames; use recvMsgParts()");
    }
    zmq_msg_close(&msg);
    return out;
}

template <int SocketType>
void ZmqBase<SocketType>::sendMsg(const std::string& msg) {
    Check(zmq_send(socket_, msg.data(), msg.size(), 0), "zmq_send");
}

template <int SocketType>
std::vector<std::string> ZmqBase<SocketType>::recvMsgParts() {
    std::vector<std::string> parts;
    while (true) {
        zmq_msg_t msg;
        Check(zmq_msg_init(&msg), "zmq_msg_init");

        const int rc = zmq_msg_recv(&msg, socket_, 0);
        if (rc == -1) {
            zmq_msg_close(&msg);
            throw std::runtime_error(std::string("zmq_msg_recv: ") + zmq_strerror(zmq_errno()));
        }

        parts.emplace_back(static_cast<const char*>(zmq_msg_data(&msg)), zmq_msg_size(&msg));
        const bool more = zmq_msg_more(&msg) != 0;
        zmq_msg_close(&msg);
        if (!more) {
            break;
        }
    }
    return parts;
}

template <int SocketType>
void ZmqBase<SocketType>::sendMsgParts(const std::vector<std::string>& parts) {
    if (parts.empty()) {
        throw std::invalid_argument("sendMsgParts(): parts is empty");
    }
    for (size_t i = 0; i < parts.size(); ++i) {
        const int flags = (i + 1 < parts.size()) ? ZMQ_SNDMORE : 0;
        const std::string& part = parts[i];
        const int rc = zmq_send(socket_, part.data(), part.size(), flags);
        if (rc == -1) {
            throw std::runtime_error(std::string("zmq_send: ") + zmq_strerror(zmq_errno()));
        }
    }
}

template <int SocketType>
void ZmqBase<SocketType>::setOption(int option, const void* value, size_t size) {
    Check(zmq_setsockopt(socket_, option, value, size), "zmq_setsockopt");
}

void ZmqSub::subscribe(const std::string& topic) {
    setOption(ZMQ_SUBSCRIBE, topic.data(), topic.size());
}

void ZmqSub::unsubscribe(const std::string& topic) {
    setOption(ZMQ_UNSUBSCRIBE, topic.data(), topic.size());
}

// Keep this list in sync with the concrete socket classes in zmq_helper.h.
// Add a line here when a new socket type is added.
template class ZmqBase<ZMQ_PUB>;
template class ZmqBase<ZMQ_SUB>;
template class ZmqBase<ZMQ_REQ>;
template class ZmqBase<ZMQ_REP>;
template class ZmqBase<ZMQ_DEALER>;
template class ZmqBase<ZMQ_ROUTER>;

}  // namespace common
