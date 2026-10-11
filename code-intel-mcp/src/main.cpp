// code-intel-mcp — codebase intelligence for coding agents.
//
// An MCP server (built on mcpp) that indexes a source tree and exposes
// symbol search, grep and paged file reading to Claude Code / Cline / Cursor.
// v1 uses fast lexical heuristics; tree-sitter parsing is the v2 upgrade
// (see docs/design.md for the plan).

#include "mcpp/server.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using namespace mcpp;

namespace {

struct Symbol {
    std::string name;
    std::string kind;  // function | class | struct | method | definition
    std::string file;  // relative path
    int64_t line = 0;  // 1-based
};

class CodeIndex {
public:
    bool empty() const { return symbols_.empty() && files_.empty(); }
    size_t file_count() const { return files_.size(); }
    size_t symbol_count() const { return symbols_.size(); }
    const std::string& root() const { return root_; }

    // Index a directory tree. Returns the number of files scanned.
    // Files are read whole; globs are inferred from extensions.
    size_t index(const std::string& root) {
        root_ = root;
        symbols_.clear();
        files_.clear();
        size_t scanned = 0;
        std::error_code ec;
        fs::path base = fs::absolute(fs::path(root), ec);
        for (fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
             it != end; it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            fs::path p = it->path();
            if (is_skipped_dir(p)) { it.disable_recursion_pending(); continue; }
            if (!is_source(p)) continue;
            std::string rel = fs::relative(p, base, ec).generic_string();
            if (ec) continue;
            FileEntry entry{rel, file_line_count(p)};
            files_.push_back(std::move(entry));
            ++scanned;
            scan_symbols(p, rel);
        }
        std::sort(symbols_.begin(), symbols_.end(), [](const Symbol& a, const Symbol& b) {
            return a.file != b.file ? a.file < b.file : a.line < b.line;
        });
        return scanned;
    }

    std::vector<Symbol> search(const std::string& query, const std::string& kind_filter,
                               size_t limit) const {
        std::string needle = to_lower(query);
        std::vector<Symbol> hits;
        for (const auto& s : symbols_) {
            if (!kind_filter.empty() && s.kind != kind_filter) continue;
            std::string hay = to_lower(s.name);
            if (hay.find(needle) != std::string::npos) {
                hits.push_back(s);
                if (hits.size() >= limit) break;
            }
        }
        return hits;
    }

    std::vector<Symbol> symbols_in_file(const std::string& rel) const {
        std::vector<Symbol> out;
        for (const auto& s : symbols_)
            if (s.file == rel) out.push_back(s);
        return out;
    }

    // Literal substring grep over indexed files. Returns "path:line: text".
    std::vector<std::string> grep(const std::string& needle, bool case_sensitive,
                                  size_t limit) const {
        std::vector<std::string> hits;
        std::string cmp_needle = case_sensitive ? needle : to_lower(needle);
        fs::path base(root_);
        std::error_code ec;
        for (const auto& f : files_) {
            std::ifstream in(base / f.path, std::ios::binary);
            if (!in) continue;
            std::string line;
            int64_t no = 0;
            while (std::getline(in, line)) {
                ++no;
                std::string hay = case_sensitive ? line : to_lower(line);
                if (hay.find(cmp_needle) != std::string::npos) {
                    std::ostringstream os;
                    os << f.path << ":" << no << ": " << line;
                    hits.push_back(os.str());
                    if (hits.size() >= limit) return hits;
                }
            }
        }
        return hits;
    }

    std::optional<std::string> read_file(const std::string& rel, int64_t start,
                                         int64_t end) const {
        fs::path p = fs::path(root_) / rel;
        if (!is_within(rel)) return std::nullopt;
        std::ifstream in(p, std::ios::binary);
        if (!in) return std::nullopt;
        std::ostringstream os;
        std::string line;
        int64_t no = 0;
        while (std::getline(in, line)) {
            ++no;
            if (no >= start && no <= end) os << no << " | " << line << "\n";
            if (no > end) break;
        }
        return os.str();
    }

private:
    struct FileEntry {
        std::string path;
        int64_t lines = 0;
    };

    static std::string to_lower(std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    static bool is_skipped_dir(const fs::path& p) {
        static const std::unordered_set<std::string> kSkip = {
            ".git", "node_modules", "build", "dist", "target", "vendor",
            ".venv", "venv", "__pycache__", ".cache", "third_party", "deps"};
        for (const auto& part : p)
            if (kSkip.count(part.generic_string())) return true;
        return false;
    }

    static bool is_source(const fs::path& p) {
        static const std::unordered_set<std::string> kExt = {
            ".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hh", ".hxx",
            ".py", ".ts", ".tsx", ".js", ".jsx", ".mjs", ".rs", ".go",
            ".java", ".kt", ".swift", ".lua", ".rb"};
        return kExt.count(p.extension().generic_string()) > 0;
    }

    static int64_t file_line_count(const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        if (!in) return 0;
        int64_t n = 0;
        std::string line;
        while (std::getline(in, line)) ++n;
        return n;
    }

    bool is_within(const std::string& rel) const {
        fs::path rp = (fs::path(root_) / rel).lexically_normal();
        std::string ps = rp.generic_string();
        std::string bs = fs::path(root_).lexically_normal().generic_string();
        return ps.rfind(bs, 0) == 0;
    }

    static std::string trim(const std::string& s) {
        size_t b = s.find_first_not_of(" \t");
        size_t e = s.find_last_not_of(" \t\r\n");
        return b == std::string::npos ? "" : s.substr(b, e - b + 1);
    }

    static bool ident_char(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    }

    // ---- lexical symbol extraction (v1; tree-sitter in v2) -------------

    void scan_symbols(const fs::path& p, const std::string& rel) {
        const std::string ext = p.extension().generic_string();
        std::ifstream in(p, std::ios::binary);
        if (!in) return;
        std::string line;
        int64_t no = 0;
        while (std::getline(in, line)) {
            ++no;
            const std::string t = trim(line);
            if (t.empty() || t[0] == '#') continue;
            if (ext == ".py") scan_python(t, rel, no);
            else if (ext == ".js" || ext == ".jsx" || ext == ".mjs" || ext == ".ts" || ext == ".tsx")
                scan_ts(t, rel, no);
            else if (ext == ".rs" || ext == ".go" || ext == ".java" || ext == ".kt" || ext == ".swift")
                scan_braced(t, rel, no);
            else
                scan_c_family(t, rel, no);
        }
    }

    static std::string extract_word_after(const std::string& t, size_t pos) {
        size_t s = t.find_first_not_of(" \t", pos);
        if (s == std::string::npos) return "";
        size_t e = s;
        while (e < t.size() && ident_char(t[e])) ++e;
        return t.substr(s, e - s);
    }

    void scan_python(const std::string& t, const std::string& rel, int64_t no) {
        const size_t indent = t.find_first_not_of(" \t");
        if (t.compare(0, 4, "def ") == 0) {
            auto name = extract_word_after(t, 3);
            if (!name.empty())
                symbols_.push_back({name, indent == 0 ? "function" : "method", rel, no});
        } else if (t.compare(0, 6, "class ") == 0) {
            auto name = extract_word_after(t, 5);
            if (!name.empty()) symbols_.push_back({name, "class", rel, no});
        }
    }

    void scan_ts(const std::string& t, const std::string& rel, int64_t no) {
        auto after_keyword = [&](const char* kw) -> std::string {
            size_t pos = t.find(kw);
            if (pos == std::string::npos) return "";
            return extract_word_after(t, pos + std::strlen(kw));
        };
        std::string name = after_keyword("function ");
        if (!name.empty()) { symbols_.push_back({name, "function", rel, no}); return; }
        name = after_keyword("class ");
        if (!name.empty()) { symbols_.push_back({name, "class", rel, no}); return; }
        // "const name = ... =>" arrow definitions
        size_t c = t.find("const ");
        if (c != std::string::npos && t.find("=>") != std::string::npos) {
            name = extract_word_after(t, c + 5);
            if (!name.empty()) symbols_.push_back({name, "definition", rel, no});
        }
    }

    void scan_braced(const std::string& t, const std::string& rel, int64_t no) {
        // Rust/Go/Java/Kotlin/Swift: "fn name(", "func name(", "name(...) {"
        for (const char* kw : {"fn ", "func ", "def "}) {
            size_t pos = t.find(kw);
            if (pos != std::string::npos) {
                std::string name = extract_word_after(t, pos + std::strlen(kw));
                if (!name.empty()) { symbols_.push_back({name, "function", rel, no}); return; }
            }
        }
        static const std::unordered_set<std::string> kControl = {
            "if", "for", "while", "switch", "catch", "return", "match", "guard"};
        size_t p = t.find('(');
        while (p != std::string::npos && p > 0) {
            size_t e = p;
            while (e > 0 && ident_char(t[e - 1])) --e;
            std::string name = t.substr(e, p - e);
            if (!name.empty() && !kControl.count(name)) {
                symbols_.push_back({name, "function", rel, no});
                return;
            }
            p = t.find('(', p + 1);
        }
    }

    void scan_c_family(const std::string& t, const std::string& rel, int64_t no) {
        // class/struct/enum declarations
        for (const char* kw : {"class ", "struct ", "enum "}) {
            size_t pos = t.find(kw);
            if (pos != std::string::npos) {
                std::string name = extract_word_after(t, pos + std::strlen(kw));
                if (!name.empty()) {
                    std::string kind(kw);
                    kind.pop_back();  // drop the trailing space
                    symbols_.push_back({name, kind, rel, no});
                    return;
                }
            }
        }
        // function-shaped lines: identifier( ... ) {  (heuristic, v1)
        static const std::unordered_set<std::string> kControl = {
            "if", "for", "while", "switch", "catch", "return", "sizeof", "defined"};
        size_t p = t.find('(');
        if (p == std::string::npos || p == 0) return;
        size_t e = p;
        while (e > 0 && ident_char(t[e - 1])) --e;
        std::string name = t.substr(e, p - e);
        if (name.empty() || kControl.count(name)) return;
        // require an opening brace on this line or the word "const" nearby to
        // cut down on false positives from prototypes and calls
        if (t.find('{') != std::string::npos || t.find("const") != std::string::npos ||
            t.back() == ')')
            symbols_.push_back({name, "function", rel, no});
    }

    std::string root_;
    std::vector<Symbol> symbols_;
    std::vector<FileEntry> files_;
};

std::string format_symbols(const std::vector<Symbol>& hits) {
    if (hits.empty()) return "(no matches)";
    std::ostringstream os;
    for (const auto& s : hits)
        os << s.kind << "  " << s.name << "  —  " << s.file << ":" << s.line << "\n";
    return os.str();
}

}  // namespace

int main() {
    CodeIndex index;

    McpServer server("com.zqy.mcpp.code-intel", "0.1.0",
                     ServerOptions{"Codebase intelligence: index a source tree, then search symbols, grep and read files. Call `index` once with the repository root before searching."});

    server.add_tool(
        "index", "Index a source tree (skips .git/node_modules/build/vendor etc.)",
        {{"root", "string", "absolute or relative path of the repository root"}},
        [&](const Json& a) {
            auto t0 = std::chrono::steady_clock::now();
            size_t files = index.index(a.get("root").as_string());
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0).count();
            std::ostringstream os;
            os << "indexed " << files << " files, " << index.symbol_count()
               << " symbols in " << ms << " ms (root: " << index.root() << ")";
            return ToolResult::text(os.str());
        });

    server.add_tool(
        "search_symbols",
        "Find symbols by substring of the name",
        {{"query", "string", "substring of symbol name"},
         {"kind", "string", "filter: function|class|struct|method|definition"},
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
            return ToolResult::text(format_symbols(index.symbols_in_file(a.get("path").as_string())));
        });

    server.add_tool(
        "grep", "Literal substring search across indexed files (case-insensitive by default)",
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
            if (!body) return ToolResult::error("cannot read file (not indexed or outside root)");
            return ToolResult::text(*body);
        });

    server.add_resource("intel://stats", "index statistics", "text/plain", [&] {
        std::ostringstream os;
        os << "files=" << index.file_count() << " symbols=" << index.symbol_count()
           << " root=" << index.root();
        return os.str();
    });

    std::fprintf(stderr, "code-intel-mcp: serving MCP on stdio\n");
    StdioTransport transport;
    server.run(transport);
    return 0;
}
