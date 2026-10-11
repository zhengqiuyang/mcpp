// examples/sysinfo_server.cpp — host information tools (cross-platform).
#include "mcpp/server.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>

namespace {

std::string iso_now() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

std::string disk_usage(const std::string& dir) {
    std::error_code ec;
    auto info = std::filesystem::space(dir.empty() ? "." : dir, ec);
    if (ec) return "ERROR: " + ec.message();
    auto gb = [](std::uintmax_t b) { return std::to_string(b / (1024ull * 1024 * 1024)) + " GiB"; };
    return "capacity=" + gb(info.capacity) + " free=" + gb(info.free) +
           " available=" + gb(info.available);
}

}  // namespace

int main() {
    using namespace mcpp;

    McpServer server("com.zqy.mcpp.example.sysinfo", "0.1.0",
                     ServerOptions{"Host information: time and disk usage."});

    server.add_tool("now", "Current UTC time (ISO-8601)", {}, [](const Json&) {
        return ToolResult::text(iso_now());
    });

    server.add_tool("disk_usage", "Disk usage for a directory",
                    {{"path", "string", "directory to inspect (default: .)"}},
                    [](const Json& a) {
                        return ToolResult::text(disk_usage(a.get_or<std::string>("path", ".")));
                    });

    server.add_tool("env", "Read an environment variable",
                    {{"name", "string", "variable name"}},
                    [](const Json& a) {
                        const char* v = std::getenv(a.get("name").as_string().c_str());
                        return ToolResult::text(v ? v : "(unset)");
                    });

    server.add_resource("sys://uptime-hint", "how to get uptime", "text/plain", [] {
        return std::string("Use the `now` tool; uptime requires platform APIs (roadmap).");
    });

    std::fprintf(stderr, "sysinfo_server: serving MCP on stdio\n");
    StdioTransport transport;
    server.run(transport);
    return 0;
}
