// mcpp/transport.hpp — message transports.
//
// MCP stdio framing is newline-delimited JSON (no Content-Length headers).
// The Transport abstraction keeps the server/client cores transport-agnostic
// so Streamable HTTP and custom buses can slot in later.

#pragma once

#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>

namespace mcpp {

class Transport {
public:
    virtual ~Transport() = default;
    // Read one framed message. Returns false on EOF / closed transport.
    virtual bool read_message(std::string& out) = 0;
    // Write one framed message.
    virtual void write_message(const std::string& msg) = 0;
};

// Newline-delimited JSON over the process stdin/stdout, per the MCP stdio
// transport spec. Messages are flushed immediately; \r is tolerated for
// CRLF-tolerant peers.
class StdioTransport : public Transport {
public:
    bool read_message(std::string& out) override {
        out.clear();
        for (;;) {
            if (!std::fgets(buf_, sizeof buf_, stdin))
                return !out.empty();  // EOF; deliver a final unterminated line
            const char* chunk = buf_;
            const char* nl = std::strchr(buf_, '\n');
            if (nl) {
                out.append(chunk, static_cast<size_t>(nl - chunk));
                break;
            }
            out.append(chunk);  // long line: buffer-sized chunk without newline
        }
        if (!out.empty() && out.back() == '\r') out.pop_back();
        return true;
    }

    void write_message(const std::string& msg) override {
        std::fwrite(msg.data(), 1, msg.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }

private:
    char buf_[65536];
};

// Single-threaded in-memory transport pair for tests and embedded wiring.
// Each end receives exactly what the other end wrote, FIFO.
class InMemoryTransport : public Transport {
public:
    void write_message(const std::string& msg) override {
        if (peer_) peer_->inbox_.push_back(msg);
    }
    bool read_message(std::string& out) override {
        if (inbox_.empty()) return false;
        out = std::move(inbox_.front());
        inbox_.pop_front();
        return true;
    }
    void connect(InMemoryTransport& other) {
        peer_ = &other;
        other.peer_ = this;
    }

private:
    InMemoryTransport* peer_ = nullptr;
    std::deque<std::string> inbox_;
};

}  // namespace mcpp
