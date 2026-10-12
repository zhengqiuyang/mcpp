// mcpp/threaded_http_transport.hpp — concurrent Streamable HTTP transport.
//
// Thread-per-connection HTTP with the full v0.3 feature set:
//   * POST  -> JSON-RPC message queued for the (still synchronous) server
//              core; replies are routed back to the requesting connection
//   * GET (Accept: text/event-stream, valid session) -> SSE stream with
//     keep-alives; server-initiated notifications (notifications/progress,
//     via Transport::write_notification) are delivered here
//   * Mcp-Session-Id sessions (mint/validate/DELETE), TTL, cap, Origin guard
//
// Concurrency model: one acceptor thread, one worker thread per connection,
// a single request queue, and a synchronous consumer (McpServer::run).
// Slow or hostile clients cannot block accept or other connections.
// stop() closes the listener and every connection, wakes the queue, and
// joins all worker threads — call it before destroying the transport.

#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "http_transport.hpp"  // Mode, Limits, socket boilerplate helpers
#include "json.hpp"
#include "transport.hpp"

namespace mcpp {

class ThreadedHttpTransport : public Transport {
public:
    using Mode = HttpTransport::Mode;
    using Limits = HttpTransport::Limits;

    ThreadedHttpTransport(uint16_t port, const std::string& bind_addr, Mode mode,
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
        acceptor_ = std::thread([this] { accept_loop(); });
    }

    ~ThreadedHttpTransport() override { stop(); }

    ThreadedHttpTransport(const ThreadedHttpTransport&) = delete;
    ThreadedHttpTransport& operator=(const ThreadedHttpTransport&) = delete;

    uint16_t port() const { return port_; }

    // Blocks until a complete request body is queued by a worker thread.
    bool read_message(std::string& out) override {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [&] { return stopping_ || !queue_.empty(); });
        if (stopping_ && queue_.empty()) return false;
        current_ = queue_.front();
        queue_.pop_front();
        out = current_.body;
        return true;
    }

    // Reply to the request last returned by read_message.
    void write_message(const std::string& msg) override {
        std::shared_ptr<Conn> conn = current_.conn.lock();
        if (!conn) return;
        std::string session_header;
        if (mode_ == Mode::Sessions && !current_.session.empty())
            session_header = "Mcp-Session-Id: " + current_.session + "\r\n";
        if (msg.empty()) {
            conn->send("HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\n" + session_header +
                       (current_.keep_alive ? "" : "Connection: close\r\n") + "\r\n");
        } else {
            char head[320];
            int n = std::snprintf(head, sizeof head,
                                  "HTTP/1.1 200 OK\r\n"
                                  "Content-Type: application/json\r\n"
                                  "Content-Length: %zu\r\n"
                                  "%s%s\r\n",
                                  msg.size(), session_header.c_str(),
                                  current_.keep_alive ? "" : "Connection: close\r\n");
            conn->send(std::string(head, static_cast<size_t>(n)) + msg);
        }
        if (!current_.keep_alive) conn->close_now();
    }

    // Server-initiated notifications ride the session's SSE streams
    // (Sessions mode). Stateless mode has no channel: dropped by design.
    void write_notification(const std::string& msg) override {
        if (msg.empty() || mode_ != Mode::Sessions) return;
        std::string sid = current_.session;
        std::vector<std::shared_ptr<Conn>> targets;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (const auto& c : sse_[sid]) targets.push_back(c);
        }
        std::string frame = "event: message\ndata: " + msg + "\n\n";
        for (auto& c : targets)
            if (!c->send(frame)) c->close_now();
    }

    void stop() {
        if (stopping_.exchange(true)) return;
        if (listener_ != kInvalid) {
#ifdef _WIN32
            ::closesocket(listener_);
#else
            ::shutdown(listener_, SHUT_RDWR);
#endif
            poke();
#ifdef _WIN32
            // closesocket above already released the descriptor
#else
            ::close(listener_);
#endif
            listener_ = kInvalid;
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto& c : conns_) c->close_now();
            cv_.notify_all();
        }
        if (acceptor_.joinable()) acceptor_.join();
        std::vector<std::thread> workers;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            workers.swap(workers_);
        }
        for (auto& w : workers)
            if (w.joinable()) w.join();
    }

private:
#ifdef _WIN32
    using SOCKET_T = SOCKET;
    static constexpr SOCKET_T kInvalid = INVALID_SOCKET;
#else
    using SOCKET_T = int;
    static constexpr SOCKET_T kInvalid = -1;
#endif

    using Clock = std::chrono::steady_clock;

    struct Conn {
        SOCKET_T fd;
        std::mutex wmtx;
        std::atomic<bool> closed{false};

        explicit Conn(SOCKET_T f) : fd(f) {}
        ~Conn() {
            if (fd != kInvalid) {
#ifdef _WIN32
                ::closesocket(fd);
#else
                ::close(fd);
#endif
            }
        }

        bool send(const std::string& bytes) {
            if (closed.load()) return false;
            std::lock_guard<std::mutex> lk(wmtx);
            size_t sent = 0;
            while (sent < bytes.size()) {
                int s = static_cast<int>(
                    ::send(fd, bytes.data() + sent, static_cast<int>(bytes.size() - sent), 0));
                if (s <= 0) return false;
                sent += static_cast<size_t>(s);
            }
            return true;
        }

        void close_now() {
            if (closed.exchange(true)) return;
#ifdef _WIN32
            ::shutdown(fd, SD_BOTH);
#else
            ::shutdown(fd, SHUT_RDWR);
#endif
        }
    };

    struct PendingRequest {
        std::string body;
        std::weak_ptr<Conn> conn;
        std::string session;
        bool keep_alive = true;
    };

    [[noreturn]] static void die(const char* what) {
        throw std::runtime_error(std::string("mcpp::ThreadedHttpTransport: ") + what);
    }

    static std::string to_lower(std::string s) {
        for (char& c : s)
            c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    static std::string find_header(const std::string& head, const std::string& name) {
        std::string lower_head = to_lower(head);
        std::string needle = "\r\n" + to_lower(name) + ":";
        size_t pos = lower_head.find(needle);
        if (pos == std::string::npos) return "";
        size_t v = head.find_first_not_of(" \t", pos + needle.size());
        if (v == std::string::npos) return "";
        size_t e = head.find("\r\n", v);
        return head.substr(v, e == std::string::npos ? std::string::npos : e - v);
    }

    static size_t parse_content_length(const std::string& head) {
        std::string v = find_header(head, "Content-Length");
        return v.empty() ? 0 : static_cast<size_t>(std::strtoull(v.c_str(), nullptr, 10));
    }

    static bool header_says_keep_alive(const std::string& head) {
        std::string conn = find_header(head, "Connection");
        if (!conn.empty()) return to_lower(conn).find("close") == std::string::npos;
        return to_lower(head).find(" http/1.1") != std::string::npos;
    }

    static bool header_contains(const std::string& head, const std::string& name,
                                const std::string& value) {
        return to_lower(find_header(head, name)).find(value) != std::string::npos;
    }

    uint16_t bound_port() const {
        sockaddr_in addr{};
        socklen_t len = sizeof addr;
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
            die("getsockname failed");
        return ntohs(addr.sin_port);
    }

    void poke() {
        if (port_ == 0) return;
        SOCKET_T poke = ::socket(AF_INET, SOCK_STREAM, 0);
        if (poke == kInvalid) return;
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

    static std::string make_session_id() {
        static unsigned counter = 0;
        auto ticks = Clock::now().time_since_epoch().count();
        char buf[48];
        std::snprintf(buf, sizeof buf, "mcp-%x-%llx", ++counter,
                      static_cast<unsigned long long>(ticks));
        return buf;
    }

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

    std::string mint_session() {
        if (limits_.session_ttl_seconds > 0) {
            auto horizon = Clock::now() - std::chrono::seconds(limits_.session_ttl_seconds);
            for (auto it = sessions_.begin(); it != sessions_.end();)
                it = it->second < horizon ? sessions_.erase(it) : std::next(it);
        }
        if (sessions_.size() >= limits_.max_sessions) return "";
        std::string id = make_session_id();
        sessions_.emplace(id, Clock::now());
        return id;
    }

    void drop_session(const std::string& sid) {
        sessions_.erase(sid);
        for (auto& c : sse_[sid]) c->close_now();
        sse_.erase(sid);
    }

    void accept_loop() {
        while (!stopping_.load()) {
            sockaddr_in peer{};
            socklen_t len = sizeof peer;
            SOCKET_T c = ::accept(listener_, reinterpret_cast<sockaddr*>(&peer), &len);
            if (c == kInvalid) {
                if (stopping_.load()) return;
                continue;
            }
            auto conn = std::make_shared<Conn>(c);
            std::thread worker([this, conn] { serve(conn); });
            {
                std::lock_guard<std::mutex> lk(mtx_);
                conns_.insert(conn);
                workers_.push_back(std::move(worker));
            }
        }
    }

    void serve(std::shared_ptr<Conn> conn) {
        std::string buf;
        while (!stopping_.load() && !conn->closed.load()) {
            // --- accumulate the HTTP head -----------------------------------
            size_t head_end;
            while ((head_end = buf.find("\r\n\r\n")) == std::string::npos) {
                if (buf.size() > 64 * 1024) return;
                char chunk[4096];
                int n = static_cast<int>(::recv(conn->fd, chunk, sizeof chunk, 0));
                if (n <= 0) return;
                buf.append(chunk, static_cast<size_t>(n));
            }
            std::string head = buf.substr(0, head_end + 4);
            buf.erase(0, head_end + 4);

            size_t sp1 = head.find(' ');
            size_t sp2 = head.find(' ', sp1 + 1);
            if (sp1 == std::string::npos || sp2 == std::string::npos) {
                conn->send("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n");
                return;
            }
            std::string method = head.substr(0, sp1);
            bool keep_alive = header_says_keep_alive(head);

            // --- DELETE ends a session ---------------------------------------
            if (method == "DELETE" && mode_ == Mode::Sessions) {
                std::string sid = find_header(head, "mcp-session-id");
                bool ok;
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    ok = session_live(sid);
                    if (ok) drop_session(sid);
                }
                conn->send(std::string("HTTP/1.1 ") + (ok ? "200 OK" : "404 Not Found") +
                           "\r\nContent-Length: 0\r\n\r\n");
                return;
            }

            // --- GET may open an SSE stream ----------------------------------
            if (method == "GET") {
                if (mode_ == Mode::Sessions &&
                    header_contains(head, "Accept", "text/event-stream")) {
                    std::string sid = find_header(head, "mcp-session-id");
                    bool ok;
                    {
                        std::lock_guard<std::mutex> lk(mtx_);
                        ok = session_live(sid);
                        if (ok) sse_[sid].push_back(conn);
                    }
                    if (ok) {
                        sse_serve(conn, sid);
                        return;
                    }
                    conn->send("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
                    return;
                }
                conn->send("HTTP/1.1 405 Method Not Allowed\r\nAllow: POST\r\n"
                           "Content-Length: 0\r\n\r\n");
                return;
            }

            if (method != "POST") {
                conn->send("HTTP/1.1 405 Method Not Allowed\r\nAllow: POST\r\n"
                           "Content-Length: 0\r\n\r\n");
                return;
            }

            // --- POST: origin guard, session bookkeeping, body ----------------
            if (mode_ == Mode::Sessions && !limits_.allowed_origin.empty()) {
                std::string origin = find_header(head, "origin");
                if (!origin.empty() && origin != limits_.allowed_origin) {
                    conn->send("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n");
                    return;
                }
            }

            size_t content_length = parse_content_length(head);
            if (content_length > 32 * 1024 * 1024) {
                conn->send("HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\n\r\n");
                return;
            }
            while (buf.size() < content_length) {
                char chunk[8192];
                int n = static_cast<int>(::recv(conn->fd, chunk, sizeof chunk, 0));
                if (n <= 0) return;
                buf.append(chunk, static_cast<size_t>(n));
            }
            std::string body = buf.substr(0, content_length);
            buf.erase(0, content_length);

            std::string sid;
            if (mode_ == Mode::Sessions) {
                bool is_initialize = false;
                try {
                    Json j = Json::parse(body);
                    is_initialize =
                        j.is_object() && j.get_or<std::string>("method", "") == "initialize";
                } catch (const Json::Error&) {
                }
                if (is_initialize) {
                    std::lock_guard<std::mutex> lk(mtx_);
                    sid = mint_session();
                    if (sid.empty()) {
                        conn->send("HTTP/1.1 503 Service Unavailable\r\n"
                                   "Content-Length: 0\r\n\r\n");
                        return;
                    }
                } else {
                    std::string presented = find_header(head, "mcp-session-id");
                    bool ok;
                    {
                        std::lock_guard<std::mutex> lk(mtx_);
                        ok = session_live(presented);
                    }
                    if (!ok) {
                        conn->send("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n");
                        return;
                    }
                    sid = presented;
                }
            }

            {
                std::lock_guard<std::mutex> lk(mtx_);
                queue_.push_back(PendingRequest{body, conn, sid, keep_alive});
                cv_.notify_one();
            }
            if (!keep_alive) return;  // reply lands via the queue; conn closes then
        }
    }

    // Hold the SSE connection open: keep-alive comments every ~15s, exit on
    // stop/session drop/send failure.
    void sse_serve(std::shared_ptr<Conn> conn, const std::string& sid) {
        conn->send(": connected\n\n");
        auto last = Clock::now();
        std::string frame = ": keepalive\n\n";
        while (!stopping_.load() && !conn->closed.load()) {
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (sessions_.find(sid) == sessions_.end()) break;
            }
            if (Clock::now() - last < std::chrono::seconds(15)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            if (!conn->send(frame)) break;
            last = Clock::now();
        }
        std::lock_guard<std::mutex> lk(mtx_);
        auto& v = sse_[sid];
        v.erase(std::remove_if(v.begin(), v.end(),
                               [&](const std::shared_ptr<Conn>& c) {
                                   return c.get() == conn.get();
                               }),
                v.end());
    }

    Mode mode_;
    Limits limits_;
    SOCKET_T listener_ = kInvalid;
    uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread acceptor_;

    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<PendingRequest> queue_;
    PendingRequest current_;
    std::set<std::shared_ptr<Conn>> conns_;
    std::vector<std::thread> workers_;
    std::map<std::string, Clock::time_point> sessions_;
    std::map<std::string, std::vector<std::shared_ptr<Conn>>> sse_;
};

}  // namespace mcpp
