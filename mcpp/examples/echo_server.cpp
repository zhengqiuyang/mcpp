// examples/echo_server.cpp — the smallest useful mcpp server.
// Try it with an MCP client such as the MCP Inspector:
//   npx @modelcontextprotocol/inspector <path-to>/echo_server
#include "mcpp/server.hpp"

#include <cctype>
#include <cstdio>
#include <string>

int main() {
    using namespace mcpp;

    McpServer server("com.zqy.mcpp.example.echo", "0.1.0",
                     ServerOptions{"A tiny demo server built with mcpp."});

    server.add_tool("echo", "Echo the message back",
                    {{"message", "string", "text to return"},
                     {"upper", "boolean", "uppercase the reply"}},
                    [](const Json& a) {
                        std::string s = a.get_or<std::string>("message", "");
                        if (a.get_or<bool>("upper", false))
                            for (char& c : s) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
                        return ToolResult::text(s);
                    });

    server.add_tool("add", "Add two integers",
                    {{"a", "integer", "left operand"}, {"b", "integer", "right operand"}},
                    [](const Json& a) {
                        return ToolResult::text(std::to_string(
                            a.get("a").as_int() + a.get("b").as_int()));
                    });

    server.add_resource("echo://about", "about this server", "text/plain",
                        [] { return std::string("mcpp echo example, built with mcpp (C++17)"); });

    std::fprintf(stderr, "echo_server: serving MCP on stdio\n");
    StdioTransport transport;
    server.run(transport);
    return 0;
}
