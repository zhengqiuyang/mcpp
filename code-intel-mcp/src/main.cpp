// code-intel-mcp — codebase intelligence for coding agents.
//
// An MCP server (built on mcpp) that indexes a source tree and exposes
// symbol search, grep and paged file reading to Claude Code / Cline / Cursor.
// Extraction engine: tree-sitter (C/C++/Python/TS/TSX) when built with
// CODE_INTEL_TREESITTER=ON, otherwise fast lexical heuristics; re-indexing
// is incremental (mtime-based).
#include "code_index.hpp"

#include "mcpp/server.hpp"
#include "mcpp/version.hpp"

#include <chrono>
#include <cstdio>
#include <iomanip>
#include <sstream>

using namespace mcpp;
using code_intel::CodeIndex;
using code_intel::IndexStats;

namespace {

std::string format_symbols(const std::vector<code_intel::Symbol>& hits) {
    if (hits.empty()) return "(no matches)";
    std::ostringstream os;
    for (const auto& s : hits)
        os << s.kind << "  " << s.name << "  —  " << s.file << ":" << s.line << "\n";
    return os.str();
}

}  // namespace

int main() {
    CodeIndex index;
    IndexStats last_stats;

    std::ostringstream about;
    about << "Codebase intelligence (mcpp " << kVersion << ", engine: "
          << (code_intel::ts_engine_available() ? "tree-sitter+lexical" : "lexical")
          << "). Call `index` once with the repository root before searching. "
             "Re-indexing is incremental (only changed files are rescanned).";

    McpServer server("com.zqy.mcpp.code-intel", "0.2.0", ServerOptions{about.str()});

    server.add_tool(
        "index",
        "Index (or incrementally re-index) a source tree; skips "
        ".git/node_modules/build/vendor etc.",
        {{"root", "string", "absolute or relative path of the repository root"}},
        [&](const Json& a) {
            auto t0 = std::chrono::steady_clock::now();
            index.index(a.get("root").as_string(), &last_stats);
            last_stats.ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0).count();
            std::ostringstream os;
            os << last_stats.scanned << " files (" << last_stats.rescanned
               << " rescanned, " << last_stats.removed << " removed), "
               << last_stats.symbols << " symbols, engine " << last_stats.engine
               << ", " << last_stats.ms << " ms (root: " << index.root() << ")";
            return ToolResult::text(os.str());
        });

    server.add_tool(
        "search_symbols", "Find symbols by substring of the name",
        {{"query", "string", "substring of symbol name"},
         {"kind", "string", "filter: function|class|struct|enum|method|definition"},
         {"limit", "integer", "max results (default 30)"}},
        [&](const Json& a) {
            auto hits = index.search(a.get("query").as_string(),
                                     a.get_or<std::string>("kind", ""),
                                     static_cast<size_t>(a.get_or<int64_t>("limit", 30)));
            return ToolResult::text(format_symbols(hits));
        });

    server.add_tool(
        "list_symbols", "List symbols defined in one indexed file",
        {{"path", "string", "repository-relative path"}},
        [&](const Json& a) {
            return ToolResult::text(
                format_symbols(index.symbols_in_file(a.get("path").as_string())));
        });

    server.add_tool(
        "grep",
        "Literal substring search across indexed files (case-insensitive by default)",
        {{"pattern", "string", "substring to find"},
         {"case_sensitive", "boolean", "match case exactly"},
         {"limit", "integer", "max results (default 50)"}},
        [&](const Json& a) {
            auto hits = index.grep(a.get("pattern").as_string(),
                                   a.get_or<bool>("case_sensitive", false),
                                   static_cast<size_t>(a.get_or<int64_t>("limit", 50)));
            if (hits.empty()) return ToolResult::text("(no matches)");
            std::ostringstream os;
            for (const auto& h : hits) os << h << "\n";
            return ToolResult::text(os.str());
        });

    server.add_tool(
        "read_file", "Read a file with line numbers (paged)",
        {{"path", "string", "repository-relative path"},
         {"start_line", "integer", "1-based first line (default 1)"},
         {"end_line", "integer", "last line inclusive (default start+200)"}},
        [&](const Json& a) {
            int64_t start = std::max<int64_t>(1, a.get_or<int64_t>("start_line", 1));
            int64_t end = a.get_or<int64_t>("end_line", start + 200);
            auto body = index.read_file(a.get("path").as_string(), start, end);
            if (!body)
                return ToolResult::error("cannot read file (not indexed or outside root)");
            return ToolResult::text(*body);
        });

    server.add_resource("intel://stats", "index statistics", "text/plain", [&] {
        std::ostringstream os;
        os << "files=" << index.file_count() << " symbols=" << index.symbol_count()
           << " engine=" << last_stats.engine << " root=" << index.root();
        return os.str();
    });

    std::fprintf(stderr, "code-intel-mcp %s (engine: %s): serving MCP on stdio\n", kVersion,
                 code_intel::ts_engine_available() ? "tree-sitter" : "lexical");
    StdioTransport transport;
    server.run(transport);
    return 0;
}
