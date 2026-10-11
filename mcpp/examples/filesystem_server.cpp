// examples/filesystem_server.cpp — sandboxed file access tools.
//
// All paths are resolved inside one root directory (MCPP_FS_ROOT or the
// current directory) with containment checks, so an LLM cannot escape the
// sandbox. Demonstrates a realistic tool surface + error reporting.
#include "mcpp/server.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path g_root;

// Resolve `user_path` inside g_root, refusing traversal outside it.
// Throws RpcError(INVALID_PARAMS) on rejection — surfaces as a clean
// JSON-RPC error rather than a tool failure.
fs::path resolve_sandboxed(const std::string& user_path) {
    fs::path base = fs::absolute(g_root).lexically_normal();
    fs::path p = (base / user_path).lexically_normal();
    std::string ps = p.generic_string();
    std::string bs = base.generic_string();
    if (ps.rfind(bs, 0) != 0 || (ps.size() > bs.size() && ps[bs.size()] != '/'))
        throw mcpp::RpcError(mcpp::rpc::INVALID_PARAMS,
                             "path escapes sandbox root: " + user_path);
    return p;
}

std::string read_file(const std::string& path) {
    fs::path p = resolve_sandboxed(path);
    std::ifstream in(p, std::ios::binary);
    if (!in) return "ERROR: cannot open " + p.generic_string();
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string list_dir(const std::string& path) {
    fs::path p = resolve_sandboxed(path.empty() ? "." : path);
    if (!fs::exists(p)) return "ERROR: no such directory: " + p.generic_string();
    std::ostringstream ss;
    for (const auto& entry : fs::directory_iterator(p)) {
        ss << (entry.is_directory() ? "[dir]  " : "[file] ")
           << entry.path().filename().generic_string();
        if (entry.is_regular_file())
            ss << "  (" << entry.file_size() << " bytes)";
        ss << "\n";
    }
    return ss.str();
}

std::string write_file(const std::string& path, const std::string& content) {
    fs::path p = resolve_sandboxed(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return "ERROR: cannot write " + p.generic_string();
    out << content;
    return "wrote " + std::to_string(content.size()) + " bytes to " + p.generic_string();
}

}  // namespace

int main() {
    using namespace mcpp;

    const char* root_env = std::getenv("MCPP_FS_ROOT");
    g_root = root_env ? fs::path(root_env) : fs::current_path();

    McpServer server("com.zqy.mcpp.example.fs", "0.1.0",
                     ServerOptions{"Sandboxed filesystem tools. All paths are relative to the root; traversal outside it is rejected."});

    server.add_tool("read_file", "Read a text file inside the sandbox",
                    {{"path", "string", "relative path"}},
                    [](const Json& a) { return ToolResult::text(read_file(a.get("path").as_string())); });

    server.add_tool("list_dir", "List a directory inside the sandbox",
                    {{"path", "string", "relative path (empty = root)"}},
                    [](const Json& a) { return ToolResult::text(list_dir(a.get("path").as_string())); });

    server.add_tool("write_file", "Write a text file inside the sandbox",
                    {{"path", "string", "relative path"},
                     {"content", "string", "file contents"}},
                    [](const Json& a) {
                        return ToolResult::text(
                            write_file(a.get("path").as_string(), a.get("content").as_string()));
                    });

    server.add_resource("fs://root", "sandbox root path", "text/plain",
                        [] { return fs::absolute(g_root).generic_string(); });

    std::fprintf(stderr, "fs_server: sandbox root = %s\n",
                 fs::absolute(g_root).generic_string().c_str());
    StdioTransport transport;
    server.run(transport);
    return 0;
}
