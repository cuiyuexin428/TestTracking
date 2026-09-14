#pragma once

#include <zmq.h>

#include <chrono>
#include <string>
#include <vector>

namespace common {

// Generic ZMQ socket wrapper. SocketType is one of the ZMQ_* constants from
// zmq.h, e.g. ZMQ_PUB, ZMQ_SUB, ZMQ_REQ, ZMQ_REP, ZMQ_DEALER, ZMQ_ROUTER.
//
// This base class exists only to share generic socket handling. It is not an
// abstract interface: use the concrete subclasses (ZmqPub, ZmqSub, ...) and
// add type-specific methods on the subclass when the base is not enough.
//
// Each instance owns one zmq context and one socket. The endpoint is passed
// at construction; call bind() on a listener or connect() on a client before
// sending/receiving. A zmq socket must not be shared between threads without
// external synchronization; use one instance per thread or lock every call
// from the outside.
template <int SocketType>
class ZmqBase {
public:
    explicit ZmqBase(const std::string& ip_port);
    virtual ~ZmqBase();

    ZmqBase(const ZmqBase&) = delete;
    ZmqBase& operator=(const ZmqBase&) = delete;

    void bind();
    void connect();

    // Blocks until the socket becomes readable/writable or the timeout
    // expires. Throws on zmq_poll errors.
    bool waitReadable(const std::chrono::milliseconds& timeout);
    bool waitWritable(const std::chrono::milliseconds& timeout);

    virtual std::string recvMsg();
    virtual void sendMsg(const std::string& msg);
    // Never wait for capacity. false retains caller ownership for retry;
    // permanent transport errors throw.
    bool trySendMsg(const std::string& msg);

    // Multi-part messages. ROUTER sockets carry an identity frame before the
    // payload; replies must echo it back as the first part.
    virtual std::vector<std::string> recvMsgParts();
    virtual void sendMsgParts(const std::vector<std::string>& parts);
    // false only before multipart starts; failure mid-message throws.
    bool trySendMsgParts(const std::vector<std::string>& parts);

    // Pass-through for zmq_setsockopt, e.g. ZMQ_RCVTIMEO, ZMQ_SUBSCRIBE.
    void setOption(int option, const void* value, size_t size);

    // Peer address ("ip:port") of the last received message, captured from
    // the ROUTER frame metadata. Empty when unavailable.
    std::string lastPeerAddress() const {
        return last_peer_address_;
    }

protected:
    void* context_ = nullptr;
    void* socket_ = nullptr;
    std::string ip_port_;
    std::string last_peer_address_;
};

// PUB broadcasts; in this role it binds, so connect() is disabled.
class ZmqPub : public ZmqBase<ZMQ_PUB> {
public:
    using ZmqBase<ZMQ_PUB>::ZmqBase;
    void connect() = delete;
};

// SUB receives broadcasts; in this role it connects, so bind() is disabled.
class ZmqSub : public ZmqBase<ZMQ_SUB> {
public:
    using ZmqBase<ZMQ_SUB>::ZmqBase;
    void bind() = delete;

    // SUB receives nothing unless it subscribes to a topic first.
    void subscribe(const std::string& topic);
    void unsubscribe(const std::string& topic);
};

class ZmqReq : public ZmqBase<ZMQ_REQ> {
public:
    using ZmqBase<ZMQ_REQ>::ZmqBase;
    void bind() = delete;
};

class ZmqRep : public ZmqBase<ZMQ_REP> {
public:
    using ZmqBase<ZMQ_REP>::ZmqBase;
    void connect() = delete;
};

// DEALER/ROUTER work on either side, so both stay enabled.
class ZmqDealer : public ZmqBase<ZMQ_DEALER> {
public:
    using ZmqBase<ZMQ_DEALER>::ZmqBase;
};

class ZmqRouter : public ZmqBase<ZMQ_ROUTER> {
public:
    using ZmqBase<ZMQ_ROUTER>::ZmqBase;
};

}  // namespace common
