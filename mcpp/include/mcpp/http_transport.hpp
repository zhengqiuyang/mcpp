// mcpp/http_transport.hpp — server-side Streamable HTTP transport (v0.1).
//
// Serves MCP over HTTP POST (the core of the Streamable HTTP transport):
//   POST /mcp  -> JSON-RPC message in body -> JSON response
//             notifications get 202 Accepted, other methods 405,
//             malformed lines still surface as JSON-RPC error responses.
//
// Deliberately v0.1: stateless (no Mcp-Session-Id), no SSE GET stream yet,
// sequential connections (no thread-per-connection). That is enough for
// local single-client deployments; keep-alive within a connection IS
// supported. Zero dependencies: raw sockets (WinSock2 / POSIX).

#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

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
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "transport.hpp"

namespace mcpp {

class HttpTransport : public Transport {
public:
    explicit HttpTransport(uint16_t port = 0, const std::string& bind_addr = "127.0.0.1") {
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) die("WSAStartup failed");
#endif
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ == kInvalid) die("socket() failed");
        int one = 1;
        ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&one), sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (::inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1)
            die("invalid bind address");
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0)
            die("bind() failed (port in use?)");
        if (::listen(listener_, 16) != 0) die("listen() failed");
        port_ = bound_port();
    }

    ~HttpTransport() override {
        close_client();
        if (listener_ != kInvalid) close_socket(listener_);
#ifdef _WIN32
        WSACleanup();
#endif
    }

    HttpTransport(const HttpTransport&) = delete;
    HttpTransport& operator=(const HttpTransport&) = delete;

    uint16_t port() const { return port_; }

    // Blocks until one HTTP POST delivers a JSON-RPC message body.
    // Non-POST requests are answered 405 internally; connection errors close
    // the client and wait for the next connection. Returns false only when
    // the listener itself is dead.
    bool read_message(std::string& out) override {
        out.clear();
        for (;;) {
            if (client_ == kInvalid && !accept_client()) return false;

            std::string head;
            if (!read_http_head(head)) {  // client closed / malformed
                close_client();
                continue;
            }

            // Request line: METHOD PATH HTTP/1.x
            size_t sp1 = head.find(' ');
            size_t sp2 = head.find(' ', sp1 + 1);
            size_t eol = head.find("\r\n");
            if (sp1 == std::string::npos || sp2 == std::string::npos || eol == std::string::npos) {
                send_simple_status("400 Bad Request", true);
                close_client();
                continue;
            }
            std::string method = head.substr(0, sp1);

            if (method != "POST") {
                // Always drop the connection on non-POST: the request may
                // carry a body we did not consume, which would desync a
                // keep-alive stream.
                send_simple_status("405 Method Not Allowed", true);
                close_client();
                continue;
            }

            size_t content_length = parse_content_length(head);
            if (content_length > 32 * 1024 * 1024) {
                send_simple_status("413 Payload Too Large", true);
                close_client();
                continue;
            }
            if (!read_exact(content_length, out)) {
                close_client();
                continue;
            }
            return true;
        }
    }

    // Reply to the last request: JSON body -> 200, empty -> 202 (notification).
    void write_message(const std::string& msg) override {
        if (client_ == kInvalid) return;
        if (msg.empty()) {
            send_simple_status("202 Accepted", false);
        } else {
            char header[256];
            int n = std::snprintf(header, sizeof header,
                                  "HTTP/1.1 200 OK\r\n"
                                  "Content-Type: application/json\r\n"
                                  "Content-Length: %zu\r\n"
                                  "%s\r\n",
                                  msg.size(), keep_alive_ ? "" : "Connection: close\r\n");
            send_all(header, static_cast<size_t>(n));
            send_all(msg.data(), msg.size());
        }
        if (!keep_alive_) close_client();
    }

private:
#ifdef _WIN32
    using SOCKET_T = SOCKET;
    static constexpr SOCKET_T kInvalid = INVALID_SOCKET;
#else
    using SOCKET_T = int;
    static constexpr SOCKET_T kInvalid = -1;
#endif

    static void close_socket(SOCKET_T s) {
#ifdef _WIN32
        ::closesocket(s);
#else
        ::close(s);
#endif
    }

    [[noreturn]] static void die(const char* what) {
        throw std::runtime_error(std::string("mcpp::HttpTransport: ") + what);
    }

    uint16_t bound_port() const {
        sockaddr_in addr{};
        socklen_t len = sizeof addr;
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
            die("getsockname failed");
        return ntohs(addr.sin_port);
    }

    bool accept_client() {
        sockaddr_in peer{};
        socklen_t len = sizeof peer;
        SOCKET_T c = ::accept(listener_, reinterpret_cast<sockaddr*>(&peer), &len);
        if (c == kInvalid) return false;
        client_ = c;
        pending_.clear();
        keep_alive_ = true;
        return true;
    }

    void close_client() {
        if (client_ != kInvalid) {
            close_socket(client_);
            client_ = kInvalid;
        }
        pending_.clear();
    }

    // Reads the HTTP head (request line + headers) from the connection
    // buffer, refilling with recv() as needed. Bytes past the head stay in
    // pending_ — the body often arrives in the same TCP segment.
    bool read_http_head(std::string& head) {
        head.clear();
        for (;;) {
            size_t end = pending_.find("\r\n\r\n");
            if (end != std::string::npos) {
                head = pending_.substr(0, end + 4);
                pending_.erase(0, end + 4);
                keep_alive_ = header_says_keep_alive(head);
                return true;
            }
            if (pending_.size() > 64 * 1024) return false;
            char chunk[4096];
            int n = static_cast<int>(::recv(client_, chunk, sizeof chunk, 0));
            if (n <= 0) return false;
            pending_.append(chunk, static_cast<size_t>(n));
        }
    }

    bool read_exact(size_t n, std::string& out) {
        out.clear();
        if (pending_.size() >= n) {
            out = pending_.substr(0, n);
            pending_.erase(0, n);
            return true;
        }
        out = pending_;
        pending_.clear();
        size_t got = out.size();
        out.resize(n);
        while (got < n) {
            int r = static_cast<int>(::recv(client_, &out[got], n - got, 0));
            if (r <= 0) return false;
            got += static_cast<size_t>(r);
        }
        return true;
    }

    bool send_all(const char* data, size_t n) {
        size_t sent = 0;
        while (sent < n) {
            int s = static_cast<int>(::send(client_, data + sent, n - sent, 0));
            if (s <= 0) return false;
            sent += static_cast<size_t>(s);
        }
        return true;
    }

    void send_simple_status(const char* status, bool include_allow) {
        if (client_ == kInvalid) return;
        std::string h = std::string("HTTP/1.1 ") + status +
                        "\r\nContent-Length: 0\r\n" +
                        (include_allow ? "Allow: POST\r\n" : "") +
                        (keep_alive_ ? "" : "Connection: close\r\n") + "\r\n";
        send_all(h.data(), h.size());
    }

    static std::string to_lower(std::string s) {
        for (char& c : s)
            c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    static size_t parse_content_length(const std::string& head) {
        size_t pos = 0;
        while ((pos = head.find("\r\n", pos)) != std::string::npos) {
            size_t line_start = pos + 2;
            size_t line_end = head.find("\r\n", line_start);
            if (line_end == std::string::npos) line_end = head.size();
            std::string line = head.substr(line_start, line_end - line_start);
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = to_lower(line.substr(0, colon));
                if (key == "content-length")
                    return static_cast<size_t>(std::strtoull(
                        line.c_str() + colon + 1, nullptr, 10));
            }
            pos = line_end;
        }
        return 0;
    }

    static bool header_says_keep_alive(const std::string& head) {
        std::string lower = to_lower(head);
        size_t conn = lower.find("\r\nconnection:");
        if (conn != std::string::npos) {
            size_t v = lower.find_first_not_of(" \t", conn + 12);
            if (v != std::string::npos && lower.compare(v, 5, "close") == 0) return false;
            return true;  // any other connection value / keep-alive
        }
        // HTTP/1.1 defaults to keep-alive; HTTP/1.0 defaults to close.
        return lower.rfind("http/1.1", 0) == 0 || lower.find(" HTTP/1.1") != std::string::npos;
    }

    SOCKET_T listener_ = kInvalid;
    SOCKET_T client_ = kInvalid;
    std::string pending_;  // read-ahead bytes for the current client
    bool keep_alive_ = true;
    uint16_t port_ = 0;
};

}  // namespace mcpp
