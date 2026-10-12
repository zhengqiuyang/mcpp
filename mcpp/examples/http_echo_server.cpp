// examples/http_echo_server.cpp — mcpp over Streamable HTTP (POST-only v0.1).
//
//   ./http_echo_server [port]        (default: 0 = pick a free port)
//
// Try it:
//   curl -s localhost:PORT/mcp -H 'Content-Type: application/json' -d @init.json
#include "mcpp/http_transport.hpp"
#include "mcpp/server.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    using namespace mcpp;

    uint16_t requested = 0;
    if (argc > 1) requested = static_cast<uint16_t>(std::atoi(argv[1]));

    McpServer server("com.zqy.mcpp.example.http-echo", "0.1.0",
                     ServerOptions{"echo/add over Streamable HTTP (POST-only, stateless v0.1)"});
    server.add_tool("echo", "Echo the message back",
                    {{"message", "string", "text to return"},
                     {"upper", "boolean", "uppercase", /*required=*/false}},
                    [](const Json& a) {
                        std::string s = a.get_or<std::string>("message", "");
                        if (a.get_or<bool>("upper", false))
                            for (char& c : s)
                                c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
                        return ToolResult::text(s);
                    });

    HttpTransport transport(requested);
    std::fprintf(stderr, "http_echo_server: POST http://127.0.0.1:%u/mcp\n",
                 static_cast<unsigned>(transport.port()));
    server.run(transport);
    return 0;
}
