#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "scheduler/ManagementServer.h"
#include "scheduler/Scheduler.h"
#include "common/Logger.h"

#include <algorithm>
#include <cctype>
#include <future>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace scheduler {
namespace {
#ifdef _WIN32
using Socket = SOCKET;
const Socket kInvalid = INVALID_SOCKET;
void Close(Socket socket) { closesocket(socket); }
#else
using Socket = int;
const Socket kInvalid = -1;
void Close(Socket socket) { close(socket); }
#endif
struct SocketOwner {
    Socket socket;
    ~SocketOwner() { if (socket != kInvalid) Close(socket); }
};
using Clock = std::chrono::steady_clock;

bool Ready(Socket socket, bool write) {
    fd_set ready;
    FD_ZERO(&ready);
    FD_SET(socket, &ready);
    timeval wait{0, 100000};
    return select(static_cast<int>(socket) + 1, write ? nullptr : &ready,
                  write ? &ready : nullptr, nullptr, &wait) > 0;
}

void SendResponse(Socket socket, int code, nlohmann::json body) {
    const auto payload = body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    const auto response = "HTTP/1.1 " + std::to_string(code) + " " +
        (code == 200 ? "OK" : "Error") + "\r\nContent-Type: application/json; charset=utf-8\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: " +
        std::to_string(payload.size()) + "\r\n\r\n" + payload;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    size_t sent = 0;
    while (sent < response.size() && Clock::now() < deadline) {
        if (!Ready(socket, true)) continue;
        const int length = static_cast<int>(std::min<size_t>(response.size() - sent, 16384));
#ifdef _WIN32
        const int count = send(socket, response.data() + sent, length, 0);
#else
        const int count = static_cast<int>(send(socket, response.data() + sent, length, MSG_NOSIGNAL));
#endif
        if (count <= 0) return;
        sent += static_cast<size_t>(count);
    }
}

bool ReadMore(Socket socket, std::string& request, Clock::time_point deadline,
              const std::atomic<bool>& running) {
    while (running.load() && Clock::now() < deadline) {
        if (!Ready(socket, false)) continue;
        char buffer[4096];
        const int count = static_cast<int>(recv(socket, buffer, sizeof(buffer), 0));
        if (count <= 0) return false;
        request.append(buffer, static_cast<size_t>(count));
        return true;
    }
    return false;
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
    return value;
}
}  // namespace

ManagementServer::~ManagementServer() { stop(); }

void ManagementServer::start(const std::string& endpoint) {
    if (endpoint.empty()) return;
    const std::string prefix = "http://127.0.0.1:";
    if (endpoint.rfind(prefix, 0) != 0)
        throw std::runtime_error("server.management_endpoint must use http://127.0.0.1:<port>");
    auto port_text = endpoint.substr(prefix.size());
    if (!port_text.empty() && port_text.back() == '/') port_text.pop_back();
    if (port_text.empty() || port_text.size() > 5 ||
        !std::all_of(port_text.begin(), port_text.end(), [](unsigned char c) { return std::isdigit(c); }))
        throw std::runtime_error("invalid management endpoint port");
    const auto port = std::stoi(port_text);
    if (port == 0) return;
    if (port > 65535) throw std::runtime_error("invalid management endpoint port");
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("management WSAStartup failed");
    socket_runtime_ = true;
#endif
    SocketOwner listener{socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    if (listener.socket == kInvalid) throw std::runtime_error("cannot create management socket");
    int exclusive = 1;
#ifdef _WIN32
    setsockopt(listener.socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#else
    setsockopt(listener.socket, SOL_SOCKET, SO_REUSEADDR, &exclusive, sizeof(exclusive));
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<unsigned short>(port));
    if (bind(listener.socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener.socket, 16) != 0)
        throw std::runtime_error("cannot listen on management endpoint " + endpoint + "; check port conflicts");
    listener_ = static_cast<std::intptr_t>(listener.socket);
    listener.socket = kInvalid;
    running_.store(true);
    thread_ = std::thread(&ManagementServer::Run, this);
    LOG_INFO("scheduler management listening: " << endpoint);
}

void ManagementServer::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    if (listener_ != -1) {
        Close(static_cast<Socket>(listener_));
        listener_ = -1;
    }
#ifdef _WIN32
    if (socket_runtime_) WSACleanup();
#endif
    socket_runtime_ = false;
}

void ManagementServer::Run() {
    std::vector<std::future<void>> requests;
    const auto listener = static_cast<Socket>(listener_);
    while (running_.load()) {
        requests.erase(std::remove_if(requests.begin(), requests.end(), [](auto& request) {
            return request.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
        }), requests.end());
        if (!Ready(listener, false)) continue;
        const auto peer = accept(listener, nullptr, nullptr);
        if (peer == kInvalid) continue;
        if (requests.size() >= 8) {
            Close(peer);
            continue;
        }
        try {
            requests.push_back(std::async(std::launch::async, [this, peer] {
                SocketOwner owner{peer};
                try { Handle(static_cast<std::intptr_t>(peer)); }
                catch (const std::exception& error) {
                    LOG_WARN("management request failed: " << error.what());
                    SendResponse(peer, 500, {{"ok", false}, {"error", "management request failed"}});
                }
            }));
        } catch (const std::exception& error) {
            Close(peer);
            LOG_WARN("cannot start management request: " << error.what());
        }
    }
    // Each request has a bounded socket deadline and command wait.
    for (auto& request : requests) request.wait();
}

void ManagementServer::Handle(std::intptr_t handle) {
    const auto socket = static_cast<Socket>(handle);
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    std::string request;
    size_t header_end;
    while ((header_end = request.find("\r\n\r\n")) == std::string::npos) {
        if (request.size() > 16384) { SendResponse(socket, 431, {{"ok", false}, {"error", "headers too large"}}); return; }
        if (!ReadMore(socket, request, deadline, running_)) return;
    }
    std::istringstream headers(request.substr(0, header_end));
    std::string method, path, version, line;
    if (!(headers >> method >> path >> version) || version.rfind("HTTP/1.", 0) != 0) {
        SendResponse(socket, 400, {{"ok", false}, {"error", "invalid HTTP request"}}); return;
    }
    std::getline(headers, line);
    size_t content_length = 0;
    bool has_length = false;
    while (std::getline(headers, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos) { SendResponse(socket, 400, {{"ok", false}, {"error", "invalid HTTP header"}}); return; }
        const auto name = Lower(line.substr(0, colon));
        if (name == "transfer-encoding") { SendResponse(socket, 400, {{"ok", false}, {"error", "transfer encoding is not supported"}}); return; }
        if (name == "content-length") {
            auto value = line.substr(colon + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            if (has_length || value.empty() || value.size() > 6 || !std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); })) {
                SendResponse(socket, 400, {{"ok", false}, {"error", "invalid content length"}}); return;
            }
            content_length = static_cast<size_t>(std::stoul(value));
            has_length = true;
        }
    }
    if (content_length > 65536) { SendResponse(socket, 413, {{"ok", false}, {"error", "request body too large"}}); return; }
    if (method == "GET" && path == "/status") { SendResponse(socket, 200, scheduler_.managementStatus()); return; }
    if (method != "POST" || path != "/commands") { SendResponse(socket, 404, {{"ok", false}, {"error", "unknown management route"}}); return; }
    if (!has_length) { SendResponse(socket, 411, {{"ok", false}, {"error", "content length required"}}); return; }
    const auto body_start = header_end + 4;
    while (request.size() - body_start < content_length) {
        if (!ReadMore(socket, request, deadline, running_)) return;
    }
    const auto body = nlohmann::json::parse(request.substr(body_start, content_length), nullptr, false);
    if (body.is_discarded()) { SendResponse(socket, 400, {{"ok", false}, {"error", "invalid JSON body"}}); return; }
    auto response = scheduler_.managementCommand(body);
    const auto code = response.value("http_status", response.value("ok", false) ? 200 : 409);
    response.erase("http_status");
    SendResponse(socket, code, std::move(response));
}
}  // namespace scheduler
