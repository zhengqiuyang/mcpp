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
#include <chrono>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

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

#include "json.hpp"
#include "transport.hpp"

namespace mcpp {

class HttpTransport : public Transport {
public:
    enum class Mode {
        Stateless,  // v0.1 behaviour: no session tracking
        Sessions,   // issue Mcp-Session-Id at initialize, require it after,
                    // honor DELETE to terminate
    };

    struct Limits {
        // Sessions expire lazily when touched after this many seconds
        // (0 = expire immediately; useful for tests).
        int session_ttl_seconds = 3600;
        // Cap on concurrent live sessions; initialize beyond it gets 503.
        size_t max_sessions = 1024;
        // When non-empty, browser requests must carry exactly this Origin
        // (protection against DNS-rebinding per the MCP spec); non-browser
        // clients that omit Origin are unaffected.
        std::string allowed_origin;
    };

    explicit HttpTransport(uint16_t port, const std::string& bind_addr, Mode mode,
                           Limits limits)
        : mode_(mode), limits_(std::move(limits)) {
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

    explicit HttpTransport(uint16_t port = 0, const std::string& bind_addr = "127.0.0.1",
                           Mode mode = Mode::Stateless)
        : HttpTransport(port, bind_addr, mode, Limits{}) {}

    ~HttpTransport() override {
        stop();
        if (listener_ != kInvalid) close_socket(listener_);
#ifdef _WIN32
        WSACleanup();
#endif
    }

    HttpTransport(const HttpTransport&) = delete;
    HttpTransport& operator=(const HttpTransport&) = delete;

    uint16_t port() const { return port_; }

    // Head (request line + headers) of the most recent request — test and
    // diagnostics hook.
    const std::string& last_request_head() const { return last_request_head_; }

    // Wake a server thread blocked in accept()/read_message() so run()
    // returns and the thread can be joined before the transport dies.
    // Only the listener is touched (the serving thread owns the client
    // socket). shutdown() reliably wakes accept on Linux, but not on all
    // BSDs — a self-connect forces the accept loop through regardless.
    void stop() {
        SOCKET_T l = listener_;
        if (l == kInvalid) return;
        listener_ = kInvalid;
        // Poke BEFORE closing: a blocked accept() is not reliably woken by
        // closesocket on Windows; the poke connection gets it unstuck.
        if (port_ != 0) {
            SOCKET_T poke = ::socket(AF_INET, SOCK_STREAM, 0);
            if (poke != kInvalid) {
                sockaddr_in a{};
                a.sin_family = AF_INET;
                a.sin_port = htons(port_);
                a.sin_addr.s_addr = htonl(0x7F000001);
                ::connect(poke, reinterpret_cast<sockaddr*>(&a), sizeof a);
#ifdef _WIN32
                ::closesocket(poke);
#else
                ::close(poke);
#endif
            }
        }
#ifdef _WIN32
        ::closesocket(l);
#else
        ::shutdown(l, SHUT_RDWR);
        ::close(l);
#endif
        // With keep-alive the serving thread may instead be blocked reading
        // the next request on an idle connection. Windows: only closesocket
        // reliably wakes a blocked recv (shutdown does not); the serving
        // thread then sees an invalid descriptor and exits without closing
        // again. POSIX: shutdown wakes it, the serving thread closes.
#ifdef _WIN32
        if (client_ != kInvalid) {
            ::closesocket(client_);
            client_ = kInvalid;
            client_released_ = true;  // serving thread must not close again
        }
#else
        if (client_ != kInvalid) ::shutdown(client_, SHUT_RDWR);
#endif
    }

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

            // DELETE ends a session (Sessions mode only).
            if (method == "DELETE" && mode_ == Mode::Sessions) {
                std::string sid = find_header(head, "mcp-session-id");
                if (session_live(sid)) {
                    sessions_.erase(sid);
                    send_simple_status("200 OK", false);
                } else {
                    send_simple_status("404 Not Found", false);
                }
                close_client();
                continue;
            }

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

            // Session bookkeeping: initialize mints a session id; every other
            // request must present a known, unexpired one. Browsers must send
            // an allowed Origin (DNS-rebinding guard).
            if (mode_ == Mode::Sessions) {
                if (!limits_.allowed_origin.empty()) {
                    std::string origin = find_header(head, "origin");
                    if (!origin.empty() && origin != limits_.allowed_origin) {
                        send_simple_status("403 Forbidden", false);
                        close_client();
                        continue;
                    }
                }
                bool is_initialize = false;
                try {
                    Json body = Json::parse(out);
                    is_initialize = body.is_object() &&
                                    body.get_or<std::string>("method", "") == "initialize";
                } catch (const Json::Error&) {
                }
                if (is_initialize) {
                    if (!mint_session()) {
                        send_simple_status("503 Service Unavailable", false);
                        close_client();
                        continue;
                    }
                } else {
                    std::string sid = find_header(head, "mcp-session-id");
                    if (!session_live(sid)) {
                        send_simple_status("400 Bad Request", false);
                        close_client();
                        continue;
                    }
                    cur_session_ = sid;
                }
            }
            return true;
        }
    }

    // Reply to the last request: JSON body -> 200, empty -> 202 (notification).
    void write_message(const std::string& msg) override {
        if (client_ == kInvalid) return;
        std::string session_header;
        if (mode_ == Mode::Sessions && !cur_session_.empty())
            session_header = "Mcp-Session-Id: " + cur_session_ + "\r\n";
        if (msg.empty()) {
            send_simple_status("202 Accepted", false);
        } else {
            char header[320];
            int n = std::snprintf(header, sizeof header,
                                  "HTTP/1.1 200 OK\r\n"
                                  "Content-Type: application/json\r\n"
                                  "Content-Length: %zu\r\n"
                                  "%s%s\r\n",
                                  msg.size(), session_header.c_str(),
                                  keep_alive_ ? "" : "Connection: close\r\n");
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
        if (client_ == kInvalid) return;
#ifdef _WIN32
        if (client_released_) {  // stop() already closed it for us
            client_released_ = false;
            client_ = kInvalid;
            pending_.clear();
            return;
        }
#endif
        // Graceful close: shutdown flushes queued response bytes as FIN
        // (a bare closesocket can emit RST and destroy the response on
        // BSD/macOS), then a bounded drain lets the peer finish its side.
#ifdef _WIN32
        DWORD ms = 250;
        ::setsockopt(client_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms),
                     sizeof ms);
#else
        timeval tv{0, 250 * 1000};
        ::setsockopt(client_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
        ::shutdown(client_,
#ifdef _WIN32
                   SD_BOTH
#else
                   SHUT_RDWR
#endif
        );
        char sink[512];
        for (int i = 0; i < 64; ++i) {
            if (::recv(client_, sink, sizeof sink, 0) <= 0) break;
        }
        close_socket(client_);
        client_ = kInvalid;
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
                last_request_head_ = head;
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

    static std::string make_session_id() {
        static unsigned counter = 0;
        auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        char buf[48];
        std::snprintf(buf, sizeof buf, "mcp-%x-%llx",
                      ++counter, static_cast<unsigned long long>(ticks));
        return buf;
    }

    using Clock = std::chrono::steady_clock;

    // Expired-on-touch validity check; false for unknown ids.
    bool session_live(const std::string& sid) {
        if (sid.empty()) return false;
        auto it = sessions_.find(sid);
        if (it == sessions_.end()) return false;
        if (limits_.session_ttl_seconds <= 0 ||
            Clock::now() - it->second > std::chrono::seconds(limits_.session_ttl_seconds)) {
            sessions_.erase(it);
            return false;
        }
        return true;
    }

    // Sweep expired sessions; mint a fresh id if under the cap.
    bool mint_session() {
        if (limits_.session_ttl_seconds > 0) {
            auto horizon = Clock::now() - std::chrono::seconds(limits_.session_ttl_seconds);
            for (auto it = sessions_.begin(); it != sessions_.end();)
                it = it->second < horizon ? sessions_.erase(it) : std::next(it);
        }
        if (sessions_.size() >= limits_.max_sessions) return false;
        cur_session_ = make_session_id();
        sessions_.emplace(cur_session_, Clock::now());
        return true;
    }

    // Case-insensitive header lookup; returns the trimmed value or "".
    static std::string find_header(const std::string& head, const std::string& name) {
        std::string lower_head = to_lower(head);
        std::string needle = "\r\n" + to_lower(name) + ":";
        size_t pos = lower_head.find(needle);
        if (pos == std::string::npos) {
            if (lower_head.rfind(to_lower(name) + ":", 0) == 0) pos = 0;  // folded start? (rare)
            else return "";
        }
        size_t v = head.find_first_not_of(" \t", pos + needle.size());
        if (v == std::string::npos) return "";
        size_t e = head.find("\r\n", v);
        return head.substr(v, e == std::string::npos ? std::string::npos : e - v);
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
        static const std::string kNeedle = "\r\nconnection:";
        size_t conn = lower.find(kNeedle);
        if (conn != std::string::npos) {
            size_t v = lower.find_first_not_of(" \t", conn + kNeedle.size());
            if (v != std::string::npos && lower.compare(v, 5, "close") == 0) return false;
            return true;  // any other connection value / keep-alive
        }
        // HTTP/1.1 defaults to keep-alive; HTTP/1.0 defaults to close.
        return lower.rfind("http/1.1", 0) == 0 || lower.find(" http/1.1") != std::string::npos;
    }

    SOCKET_T listener_ = kInvalid;
    SOCKET_T client_ = kInvalid;
    std::string pending_;  // read-ahead bytes for the current client
    bool keep_alive_ = true;
    uint16_t port_ = 0;
    Mode mode_ = Mode::Stateless;
    Limits limits_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> sessions_;
    std::string cur_session_;
    std::string last_request_head_;
    bool client_released_ = false;  // stop() closed client_ (Windows wake)
};

}  // namespace mcpp
