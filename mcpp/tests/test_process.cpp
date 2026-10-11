// Integration test for mcpp/process_transport.hpp — spawns a real child
// process through OS pipes on both Windows and POSIX, then (when the bundled
// echo_server is available via MCPP_ECHO_SERVER) drives a full MCP session.
#include "mcpp/client.hpp"
#include "mcpp/process_transport.hpp"
#include "test_util.hpp"

#include <cstdlib>
#include <string>
#include <vector>

using namespace mcpp;

static void test_spawn_echo_child() {
#ifdef _WIN32
    std::vector<std::string> cmd{"cmd.exe", "/c", "echo hello-from-child"};
#else
    std::vector<std::string> cmd{"/bin/echo", "hello-from-child"};
#endif
    ProcessTransport transport(cmd);
    std::string line;
    CHECK(transport.read_message(line));
    CHECK_EQ(line, std::string("hello-from-child"));
    // next read hits EOF: the child has exited
    CHECK(!transport.read_message(line));
}

static void test_full_session_against_echo_server() {
    const char* exe = std::getenv("MCPP_ECHO_SERVER");
    if (!exe || !*exe) return;  // not run from CMake; skip silently

    ProcessTransport transport{{exe}};
    McpClient client(transport);

    auto info = client.initialize("proc-test", "1.0");
    CHECK_EQ(info.name, std::string("com.zqy.mcpp.example.echo"));

    auto tools = client.list_tools();
    CHECK_EQ(tools.size(), static_cast<size_t>(2));

    auto r = client.call_tool("add", Json{{"a", 40}, {"b", 2}});
    CHECK_EQ(r.text_content(), std::string("42"));
    CHECK_EQ(r.is_error, false);
}

int main() {
    test_spawn_echo_child();
    test_full_session_against_echo_server();
    std::printf("test_process: %d checks, %d failures\n", mcpptest::g_checks, mcpptest::g_failures);
    return mcpptest::g_failures == 0 ? 0 : 1;
}
