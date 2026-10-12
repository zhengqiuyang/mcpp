// ThreadedHttpTransport integration: POST round-trips, SSE GET streams that
// carry server-initiated notifications (progress), sessions, and immunity to
// stalled clients.
#include "mcpp/client.hpp"
#include "mcpp/http_client_transport.hpp"
#include "mcpp/server.hpp"
#include "mcpp/threaded_http_transport.hpp"
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
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
struct WsaGuard {};
using SocketT = int;
constexpr SocketT kBad = -1;
void sock_close(SocketT s) { close(s); }
#endif

SocketT connect_local(uint16_t port) {
    SocketT s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(0x7F000001);
    ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
    return s;
}

bool wait_readable(SocketT s, int ms) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(s, &rf);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
#ifdef _WIN32
    return ::select(static_cast<int>(s), &rf, nullptr, nullptr, &tv) > 0;
#else
    return ::select(static_cast<int>(s) + 1, &rf, nullptr, nullptr, &tv) > 0;
#endif
}

std::string read_until(SocketT s, int total_ms) {
    std::string out;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(total_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!wait_readable(s, 100)) continue;
        char buf[4096];
        int n = ::recv(s, buf, sizeof buf, 0);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

}  // namespace

int main() {
    WsaGuard guard;

    McpServer server("threaded.test", "1.0");
    server.add_tool("add", "Add two integers",
                    {{"a", "integer", "l"}, {"b", "integer", "r"}},
                    [](const Json& a) {
                        return ToolResult::text(std::to_string(a.get("a").as_int() +
                                                               a.get("b").as_int()));
                    });
    server.add_tool(
        "slow", "Reports progress while working",
        {{"steps", "integer", "how many steps"}},
        [](const Json& a, ToolContext& ctx) {
            int64_t steps = a.get("steps").as_int();
            for (int64_t i = 1; i <= steps; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                ctx.report_progress(i, steps, "working");
            }
            return ToolResult::text("done");
        });
    server.handle_message(msg::notification("notifications/initialized"));

    ThreadedHttpTransport transport(0, "127.0.0.1", ThreadedHttpTransport::Mode::Sessions,
                                    ThreadedHttpTransport::Limits{});
    std::thread server_thread([&] { server.run(transport); });
    const uint16_t port = transport.port();

    // ---- 1) basic round-trip with sessions ------------------------------
    {
        HttpClientTransport ct("http://127.0.0.1:" + std::to_string(port) + "/mcp");
        McpClient client(ct);
        auto info = client.initialize("threaded-test", "1.0");
        CHECK_EQ(info.name, std::string("threaded.test"));
        CHECK(ct.session_id().rfind("mcp-", 0) == 0);
        auto r = client.call_tool("add", Json{{"a", 40}, {"b", 2}});
        CHECK_EQ(r.text_content(), std::string("42"));

        // ---- 2) SSE stream carries progress notifications ------------------
        SocketT sse = connect_local(port);
        CHECK(sse != kBad);
        std::string get_req = "GET /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                              "Accept: text/event-stream\r\nMcp-Session-Id: " +
                              ct.session_id() + "\r\n\r\n";
        ::send(sse, get_req.data(), static_cast<int>(get_req.size()), 0);
        std::string hello = read_until(sse, 1500);
        CHECK(hello.find(": connected") != std::string::npos);

        // trigger the slow tool with a progress token; the SSE stream must
        // receive the notifications while the POST response is pending
        Json params = Json::object();
        params["name"] = "slow";
        params["arguments"]["steps"] = 3;
        params["_meta"]["progressToken"] = 55;
        Json result = client.call("tools/call", std::move(params));
        CHECK_EQ(result.get_or<bool>("isError", false), false);

        std::string events = read_until(sse, 2500);
        CHECK(events.find("event: message") != std::string::npos);
        CHECK(events.find("notifications/progress") != std::string::npos);
        CHECK(events.find("\"progressToken\":55") != std::string::npos);
        size_t progress_events = 0;
        for (size_t pos = events.find("notifications/progress"); pos != std::string::npos;
             pos = events.find("notifications/progress", pos + 1))
            ++progress_events;
        CHECK_EQ(progress_events, static_cast<size_t>(3));
        sock_close(sse);

        // ---- 3) a stalled client cannot block others -----------------------
        SocketT stalled = connect_local(port);
        std::string partial = "POST /mcp HTTP/1.1\r\nContent-Length: 1000\r\n\r\nhi";
        ::send(stalled, partial.data(), static_cast<int>(partial.size()), 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto r2 = client.call_tool("add", Json{{"a", 1}, {"b", 1}});
        CHECK_EQ(r2.text_content(), std::string("2"));
        sock_close(stalled);
    }

    transport.stop();
    server_thread.join();

    std::printf("test_http_threaded: %d checks, %d failures\n", mcpptest::g_checks,
                mcpptest::g_failures);
    return mcpptest::g_failures == 0 ? 0 : 1;
}
