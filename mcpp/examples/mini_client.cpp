// examples/mini_client.cpp — spawn any MCP stdio server and talk to it.
//
//   ./mini_client ./echo_server
//   ./mini_client ./echo_server --call echo --args '{"message":"hi","upper":true}'
//
// ProcessTransport now lives in the library (mcpp/process_transport.hpp);
// this example is a thin CLI over McpClient for quick manual checks.
#include "mcpp/client.hpp"
#include "mcpp/process_transport.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace mcpp;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: mini_client <server-command> [args...] [--call <tool> --args '<json>']\n");
        return 2;
    }
    std::vector<std::string> command;
    std::string call_tool, call_args = "{}";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--call" && i + 1 < argc) call_tool = argv[++i];
        else if (a == "--args" && i + 1 < argc) call_args = argv[++i];
        else command.push_back(std::move(a));
    }

    ProcessTransport transport(command);
    McpClient client(transport);

    auto info = client.initialize("mcpp-mini-client", "0.1.0");
    std::printf("connected: %s v%s (protocol %s)\n", info.name.c_str(),
                info.version.c_str(), client.negotiated_protocol_version().c_str());

    auto tools = client.list_tools();
    std::printf("%zu tools:\n", tools.size());
    for (const auto& t : tools) std::printf("  - %-16s %s\n", t.name.c_str(), t.description.c_str());

    if (!call_tool.empty()) {
        try {
            Json args = Json::parse(call_args);
            ToolResult r = client.call_tool(call_tool, std::move(args));
            std::printf("\n%s %s:\n%s\n", call_tool.c_str(),
                        r.is_error ? "(error)" : "(ok)", r.text_content().c_str());
        } catch (const McpRemoteError& e) {
            std::printf("\n%s failed: [%d] %s\n", call_tool.c_str(), e.code, e.what());
        }
    }
    return 0;
}
