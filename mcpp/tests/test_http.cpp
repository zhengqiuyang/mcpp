// End-to-end test: McpClient over HttpClientTransport against McpServer over
// HttpTransport, on a real loopback TCP socket. Validates the full Streamable
// HTTP (POST-only) path: initialize, notifications, tools/list, tools/call
// and remote errors.
#include "mcpp/client.hpp"
#include "mcpp/http_client_transport.hpp"
#include "mcpp/threaded_http_transport.hpp"
#include "mcpp/server.hpp"
#include "test_util.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace mcpp;

int main() {
    // Server: bound to a free port; the listener exists before the thread
    // starts, so the client can connect immediately.
    McpServer server("http.test.server", "1.0.0");
    server.add_tool("add", "Add two integers",
                    {{"a", "integer", "left"}, {"b", "integer", "right"}},
                    [](const Json& a) {
                        return ToolResult::text(std::to_string(a.get("a").as_int() +
                                                               a.get("b").as_int()));
                    });

    // Threaded transport: the test drives two keep-alive clients at once,
    // which the single-threaded accept loop cannot multiplex.
    ThreadedHttpTransport server_transport(0, "127.0.0.1",
                                           ThreadedHttpTransport::Mode::Stateless,
                                           ThreadedHttpTransport::Limits{});
    const uint16_t port = server_transport.port();
    CHECK(port != 0);

    std::thread server_thread([&] { server.run(server_transport); });

    HttpClientTransport client_transport("http://127.0.0.1:" + std::to_string(port) + "/mcp");
    McpClient client(client_transport);

    auto info = client.initialize("http-test", "1.0");
    CHECK_EQ(info.name, std::string("http.test.server"));

    auto tools = client.list_tools();
    CHECK_EQ(tools.size(), static_cast<size_t>(1));
    CHECK_EQ(tools.at(0).name, std::string("add"));

    auto r = client.call_tool("add", Json{{"a", 40}, {"b", 2}});
    CHECK_EQ(r.text_content(), std::string("42"));
    CHECK_EQ(r.is_error, false);

    bool threw = false;
    try {
        client.call_tool("nope");
    } catch (const McpRemoteError& e) {
        threw = true;
        CHECK_EQ(e.code, rpc::INVALID_PARAMS);
    }
    CHECK(threw);

    // A second session on a fresh connection still sees the initialized
    // server (process-wide readiness, matching the stateless HTTP model).
    {
        HttpClientTransport t2("http://127.0.0.1:" + std::to_string(port) + "/");
        McpClient c2(t2);
        auto r2 = c2.call_tool("add", Json{{"a", 1}, {"b", 1}});
        CHECK_EQ(r2.text_content(), std::string("2"));
    }

    server_transport.stop();
    server_thread.join();
    std::printf("test_http: %d checks, %d failures\n", mcpptest::g_checks, mcpptest::g_failures);
    return mcpptest::g_failures == 0 ? 0 : 1;
}
