// HTTP hardening tests: session TTL expiry, session-count cap, and the
// Origin guard (DNS-rebinding protection). Drives the server with a raw
// socket client so hostile request shapes can be sent verbatim.
#include "mcpp/http_transport.hpp"
#include "mcpp/server.hpp"
#include "test_util.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace mcpp;

namespace {

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
struct WsaGuard {
    WsaGuard() { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); }
    ~WsaGuard() { WSACleanup(); }
};
using SocketT = SOCKET;
constexpr SocketT kBad = INVALID_SOCKET;
void sock_close(SocketT s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
struct WsaGuard {};
using SocketT = int;
constexpr SocketT kBad = -1;
void sock_close(SocketT s) { close(s); }
#endif

// Send one raw request, read until the peer closes, return the raw response.
std::string raw_exchange(uint16_t port, const std::string& request) {
    SocketT s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kBad) return "(socket failed)";
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(0x7F000001);
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        sock_close(s);
        return "(connect failed)";
    }
    ::send(s, request.data(), static_cast<int>(request.size()), 0);
    std::string out;
    char buf[4096];
    int n;
    while ((n = ::recv(s, buf, sizeof buf, 0)) > 0) out.append(buf, static_cast<size_t>(n));
    sock_close(s);
    return out;
}

std::string post(const std::string& body, const std::string& extra_headers = "") {
    return "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
           "Connection: close\r\n" + extra_headers + "Content-Length: " +
           std::to_string(body.size()) + "\r\n\r\n" + body;
}

int status_of(const std::string& response) {
    size_t sp = response.find(' ');
    return sp == std::string::npos ? 0 : std::atoi(response.c_str() + sp + 1);
}

const char* kInit =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
    "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},\"clientInfo\":{\"name\":\"t\",\"version\":\"0\"}}}";
const char* kPing = "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"}";

McpServer make_server() {
    McpServer server("hardening.test", "1.0");
    server.add_tool("add", "Add two integers",
                    {{"a", "integer", "l"}, {"b", "integer", "r"}},
                    [](const Json& a) {
                        return ToolResult::text(std::to_string(a.get("a").as_int() +
                                                               a.get("b").as_int()));
                    });
    return server;
}

}  // namespace

static void test_session_ttl() {
    std::fprintf(stderr, ">> ttl\n");
    auto server = make_server();
    HttpTransport::Limits lim;
    lim.session_ttl_seconds = 0;  // expire immediately
    HttpTransport transport(0, "127.0.0.1", HttpTransport::Mode::Sessions, lim);
    std::thread th([&] { server.run(transport); });

    std::string init_resp = raw_exchange(transport.port(), post(kInit));
    CHECK_EQ(status_of(init_resp), 200);

    // the session expired on touch: a follow-up with its id is rejected
    size_t h = init_resp.find("Mcp-Session-Id: ");
    CHECK(h != std::string::npos);
    std::string sid = init_resp.substr(h + 16, init_resp.find("\r\n", h) - (h + 16));
    std::string follow =
        raw_exchange(transport.port(), post(kPing, "Mcp-Session-Id: " + sid + "\r\n"));
    CHECK_EQ(status_of(follow), 400);
    transport.stop();
    th.join();
}

static void test_session_cap() {
    std::fprintf(stderr, ">> cap\n");
    auto server = make_server();
    HttpTransport::Limits lim;
    lim.max_sessions = 1;
    HttpTransport transport(0, "127.0.0.1", HttpTransport::Mode::Sessions, lim);
    std::thread th([&] { server.run(transport); });

    CHECK_EQ(status_of(raw_exchange(transport.port(), post(kInit))), 200);
    CHECK_EQ(status_of(raw_exchange(transport.port(), post(kInit))), 503);  // cap
    transport.stop();
    th.join();
}

static void test_origin_guard() {
    std::fprintf(stderr, ">> origin\n");
    auto server = make_server();
    HttpTransport::Limits lim;
    lim.allowed_origin = "http://localhost:3000";
    HttpTransport transport(0, "127.0.0.1", HttpTransport::Mode::Sessions, lim);
    std::thread th([&] { server.run(transport); });

    // hostile browser origin -> 403
    CHECK_EQ(status_of(raw_exchange(transport.port(),
                                    post(kInit, "Origin: http://evil.example\r\n"))),
             403);
    // allowed browser origin -> 200
    CHECK_EQ(status_of(raw_exchange(transport.port(),
                                    post(kInit, "Origin: http://localhost:3000\r\n"))),
             200);
    // non-browser clients (no Origin header) are unaffected
    CHECK_EQ(status_of(raw_exchange(transport.port(), post(kPing))), 400);  // no session yet, but not 403
    transport.stop();
    th.join();
}

int main() {
    WsaGuard guard;
    test_session_ttl();
    test_session_cap();
    test_origin_guard();
    std::printf("test_http_hardening: %d checks, %d failures\n", mcpptest::g_checks,
                mcpptest::g_failures);
    return mcpptest::g_failures == 0 ? 0 : 1;
}
