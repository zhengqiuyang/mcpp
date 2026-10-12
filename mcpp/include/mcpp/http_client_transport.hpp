// mcpp/http_client_transport.hpp — client side of the Streamable HTTP
// transport (v0.1). One connection per message (Connection: close), which
// matches the stateless server transport and keeps the implementation
// dependency-free: raw sockets, no TLS (http:// only; wrap with a local
// proxy for https).

#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "transport.hpp"

namespace mcpp {

class HttpClientTransport : public Transport {
public:
    // url: http://host[:port]/path — https:// is rejected (add a local TLS
    // proxy or wait for the TLS-enabled v0.2).
    explicit HttpClientTransport(const std::string& url) {
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) die("WSAStartup failed");
#endif
        if (url.rfind("http://", 0) != 0) die("only http:// URLs are supported in v0.1");
        size_t host_start = 7;
        size_t path_start = url.find('/', host_start);
        if (path_start == std::string::npos) {
            host_port_ = url.substr(host_start);
            path_ = "/";
        } else {
            host_port_ = url.substr(host_start, path_start - host_start);
            path_ = url.substr(path_start);
        }
        size_t colon = host_port_.find(':');
        if (colon == std::string::npos) {
            host_ = host_port_;
            port_ = 80;
        } else {
            host_ = host_port_.substr(0, colon);
            port_ = static_cast<uint16_t>(std::atoi(host_port_.c_str() + colon + 1));
        }
        if (host_.empty()) die("empty host in URL");
    }

    ~HttpClientTransport() override {
        close_socket_if_open();
#ifdef _WIN32
        WSACleanup();
#endif
    }

    HttpClientTransport(const HttpClientTransport&) = delete;
    HttpClientTransport& operator=(const HttpClientTransport&) = delete;

    // Session assigned by the server (Sessions mode); empty in stateless mode.
    const std::string& session_id() const { return session_id_; }
    // Raw response head of the last read (diagnostics/tests).
    const std::string& last_response_headers() const { return last_headers_; }

    // Attach an extra header to every subsequent request (e.g.
    // "Authorization: Bearer <key>" for LLM APIs). Appends, not replaces.
    void set_header(std::string name, std::string value) {
        extra_headers_.emplace_back(std::move(name), std::move(value));
    }

    // Terminate a session (DELETE). No-op when no session is held.
    void end_session() {
        if (session_id_.empty()) return;
        sent_ = false;
        send_request("DELETE", /*with_body=*/false);
        std::string head;
        char chunk[2048];
        while (head.find("\r\n\r\n") == std::string::npos) {
            int n = static_cast<int>(::recv(sock_, chunk, sizeof chunk, 0));
            if (n <= 0) break;
            head.append(chunk, static_cast<size_t>(n));
        }
        close_socket_if_open();
        session_id_.clear();
    }

    // Sends the queued POST immediately. Requests are answered by
    // read_message. The connection is kept alive across messages when the
    // server allows it (per-message connections rapidly reuse the same
    // loopback 4-tuple and can receive stale RSTs from the kernel).
    void write_message(const std::string& msg) override {
        pending_ = msg;
        sent_ = false;
        send_pending();
    }

    // HTTP answers every POST (notifications get 202); drain that answer so
    // nothing is left unread on the persistent connection.
    void drain_response() override {
        if (!sent_) return;
        std::string scratch;
        read_one(scratch);
    }

    // Reads the response body (HTTP 200) for the pending request — or
    // returns false on 202 (notification accepted: nothing for the client).
    bool read_message(std::string& out) override {
        out.clear();
        if (!send_pending()) return false;
        return read_one(out);
    }

    // Reads one already-sent request's response from the live connection.
    bool read_one(std::string& out) {
        std::string head;
        char chunk[8192];
        while (head.find("\r\n\r\n") == std::string::npos) {
            int n = static_cast<int>(::recv(sock_, chunk, sizeof chunk, 0));
            if (n <= 0) {
                close_socket_if_open();
                return false;
            }
            head.append(chunk, static_cast<size_t>(n));
        }
        size_t body_at = head.find("\r\n\r\n") + 4;
        last_headers_ = head.substr(0, body_at);

        // Capture an assigned session id (Mcp-Session-Id response header).
        size_t sid = to_lower_copy(head).find("\r\nmcp-session-id:");
        if (sid != std::string::npos) {
            size_t v = head.find_first_not_of(" \t", sid + 17);
            size_t e = head.find("\r\n", v);
            if (v != std::string::npos)
                session_id_ = head.substr(v, e == std::string::npos ? std::string::npos
                                                                    : e - v);
        }

        // status line: HTTP/1.1 200 OK
        int status = std::atoi(head.c_str() + head.find(' ') + 1);
        bool server_closes = to_lower_copy(head).find("\r\nconnection: close") !=
                             std::string::npos;
        if (status == 202) {  // notification accepted: nothing for the client
            if (server_closes) close_socket_if_open();
            return false;
        }
        if (status != 200) {
            close_socket_if_open();
            throw std::runtime_error("mcpp::HttpClientTransport: HTTP " +
                                     std::to_string(status));
        }

        size_t content_length = content_length_of(head);
        std::string body = head.substr(body_at);
        if (body.size() > content_length) body.resize(content_length);
        const size_t preloaded = body.size();
        body.resize(content_length);  // grow FIRST so recv writes inside bounds
        size_t filled = preloaded;
        while (filled < content_length) {
            int n = static_cast<int>(
                ::recv(sock_, &body[filled], content_length - filled, 0));
            if (n <= 0) {
                close_socket_if_open();
                return false;
            }
            filled += static_cast<size_t>(n);
        }
        out = std::move(body);
        // Honour the server's connection preference: keep the socket for the
        // next message unless it asked us to close.
        if (server_closes) close_socket_if_open();
        return true;
    }

private:
#ifdef _WIN32
    using SOCKET_T = SOCKET;
    static constexpr SOCKET_T kInvalid = INVALID_SOCKET;
#else
    using SOCKET_T = int;
    static constexpr SOCKET_T kInvalid = -1;
#endif

    [[noreturn]] static void die(const char* what) {
        throw std::runtime_error(std::string("mcpp::HttpClientTransport: ") + what);
    }

    static void close_socket_raw(SOCKET_T s) {
#ifdef _WIN32
        ::closesocket(s);
#else
        ::close(s);
#endif
    }

    // Graceful teardown: shutdown + bounded drain before closesocket.
    // Closing with unread bytes in the receive buffer makes the kernel emit
    // RST (observed breaking the NEXT connection on loopback), so every
    // close drains first.
    void close_socket_if_open() {
        if (sock_ == kInvalid) return;
#ifdef _WIN32
        DWORD ms = 250;
        ::setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms),
                     sizeof ms);
#else
        timeval tv{0, 250 * 1000};
        ::setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
        ::shutdown(sock_,
#ifdef _WIN32
                   SD_BOTH
#else
                   SHUT_RDWR
#endif
        );
        char sink[512];
        for (int i = 0; i < 64; ++i) {
            if (::recv(sock_, sink, sizeof sink, 0) <= 0) break;
        }
        close_socket_raw(sock_);
        sock_ = kInvalid;
    }

    bool send_pending() {
        if (sent_) return true;
        send_request("POST", /*with_body=*/true);
        sent_ = true;
        return true;
    }

    // Writes the request head (+ body for POST) on the current connection,
    // establishing one first when needed. Every call transmits a full
    // request; only the connection setup is conditional on reuse.
    void send_request(const char* method, bool with_body) {
        if (sock_ == kInvalid) {  // establish the keep-alive connection
            close_socket_if_open();
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo* res = nullptr;
            if (::getaddrinfo(host_.c_str(), std::to_string(port_).c_str(), &hints, &res) != 0 ||
                !res)
                die("cannot resolve host");
            sock_ = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
            if (sock_ == kInvalid) { ::freeaddrinfo(res); die("socket() failed"); }
            if (::connect(sock_, res->ai_addr, res->ai_addrlen) != 0) {
                ::freeaddrinfo(res);
                close_socket_if_open();
                die("connect() failed");
            }
            ::freeaddrinfo(res);
        }

        std::string hdr = std::string(method) + " " + path_ + " HTTP/1.1\r\n" +
                          "Host: " + host_port_ + "\r\n" +
                          "Content-Type: application/json\r\n" +
                          "Accept: application/json\r\n";  // HTTP/1.1: keep-alive default
        if (!session_id_.empty()) hdr += "Mcp-Session-Id: " + session_id_ + "\r\n";
        for (const auto& [name, value] : extra_headers_)
            hdr += name + ": " + value + "\r\n";
        hdr += "Content-Length: " +
               std::to_string(with_body ? pending_.size() : size_t(0)) + "\r\n\r\n";
        send_all(hdr.data(), hdr.size());
        if (with_body) send_all(pending_.data(), pending_.size());
    }

    bool send_all(const char* data, size_t n) {
        size_t sent = 0;
        while (sent < n) {
            int s = static_cast<int>(::send(sock_, data + sent, n - sent, 0));
            if (s <= 0) return false;
            sent += static_cast<size_t>(s);
        }
        return true;
    }

    static size_t content_length_of(const std::string& head) {
        std::string lower = to_lower_copy(head);
        size_t pos = lower.find("content-length:");
        if (pos == std::string::npos) return 0;
        return static_cast<size_t>(std::strtoull(head.c_str() + pos + 15, nullptr, 10));
    }

    static std::string to_lower_copy(std::string s) {
        for (char& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    std::string url_, host_, host_port_, path_ = "/", pending_;
    std::string session_id_;
    std::string last_headers_;
    std::vector<std::pair<std::string, std::string>> extra_headers_;
    uint16_t port_ = 80;
    SOCKET_T sock_ = kInvalid;
    bool sent_ = false;
};

}  // namespace mcpp
