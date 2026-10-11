// tests/bench.cpp — micro-benchmarks backing mcpp's performance claims.
// Run: ./bench   (single-threaded, ~10 seconds)
#include "mcpp/client.hpp"
#include "mcpp/server.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <string>

using namespace mcpp;
using Clock = std::chrono::steady_clock;

// Sinks prevent the optimizer from discarding the work without resorting to
// `volatile` on non-trivial types.
Json g_json_sink;
std::string g_str_sink;
std::optional<Json> g_opt_sink;
ToolResult g_tool_sink;

static void bench(const char* name, int iters, const std::function<void()>& fn) {
    auto t0 = Clock::now();
    fn();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    double ops = iters / (ms / 1000.0);
    std::printf("%-42s %9.0f ops/s  (%d iters in %lld ms)\n", name, ops, iters,
                static_cast<long long>(ms));
}

// A realistic tools/call request — the hot path of every MCP session.
static const char* kCallJson =
    "{\"jsonrpc\":\"2.0\",\"id\":123,\"method\":\"tools/call\",\"params\":{\"name\":\"add\","
    "\"arguments\":{\"a\":19,\"b\":23},\"_meta\":{\"progressToken\":7}}}";

static McpServer g_server("bench.server", "1.0");

int main() {
    const int kParseIters = 200000;
    bench("Json::parse (MCP tools/call, ~150B)", kParseIters, [] {
        for (int i = 0; i < kParseIters; ++i) g_json_sink = Json::parse(kCallJson);
    });

    const Json parsed = Json::parse(kCallJson);
    bench("Json::dump  (same message)", kParseIters, [&] {
        for (int i = 0; i < kParseIters; ++i) g_str_sink = parsed.dump();
    });

    // Server-side dispatch without transport serialization.
    g_server.add_tool("add", "Add two integers",
                      {{"a", "integer", "left"}, {"b", "integer", "right"}},
                      [](const Json& a) {
                          return ToolResult::text(std::to_string(a.get("a").as_int() +
                                                                 a.get("b").as_int()));
                      });
    g_server.handle_message(msg::notification("notifications/initialized"));
    const Json req = Json::parse(kCallJson);

    const int kDispatchIters = 200000;
    bench("McpServer::handle_message (dispatch)", kDispatchIters, [&] {
        for (int i = 0; i < kDispatchIters; ++i) g_opt_sink = g_server.handle_message(req);
    });

    // Full wire round-trip: serialize -> transport -> parse -> dispatch ->
    // serialize -> transport -> parse, from the client's perspective.
    static InMemoryTransport client_end, server_end;
    client_end.connect(server_end);
    static McpClient client(client_end);
    client.set_idle_hook([&] {
        std::string l;
        bool delivered = false;
        while (server_end.read_message(l)) {
            auto reply = g_server.handle_line(l);
            if (reply) {
                server_end.write_message(*reply);
                delivered = true;
            }
        }
        return delivered;
    });
    client.initialize("bench", "1.0");

    const int kRoundIters = 50000;
    bench("full round-trip (wire <-> dispatch)", kRoundIters, [] {
        for (int i = 0; i < kRoundIters; ++i)
            g_tool_sink = client.call_tool("add", Json{{"a", 19}, {"b", 23}});
    });

    std::printf("\nmessage size: %zu bytes; single thread; mcpp v0.1\n", std::strlen(kCallJson));
    return 0;
}
