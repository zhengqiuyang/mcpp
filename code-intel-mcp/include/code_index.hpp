// code_index.hpp — the indexing core of code-intel-mcp, independent of MCP
// wiring so it can be unit-tested directly.
//
// Two extraction engines:
//   * lexical heuristics (always available, zero dependencies)
//   * tree-sitter (compiled in with MCPP_CODE_INTEL_TREESITTER; precise
//     symbols for C / C++ / Python / TypeScript / TSX)
// Incremental re-index: files whose mtime is unchanged keep their symbols.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace code_intel {

struct Symbol {
    std::string name;
    std::string kind;  // function | method | class | struct | enum | definition
    std::string file;  // repository-relative path
    int64_t line = 0;  // 1-based
    std::string name_lower;  // precomputed for case-insensitive search
};

struct Reference {
    std::string file;
    int64_t line = 0;
    bool is_definition = false;
    std::string text;  // trimmed source line
};

// tree-sitter based extraction (implemented in ts_symbols.cpp; only linked
// when built with MCPP_CODE_INTEL_TREESITTER). Returns nullopt when the
// extension has no tree-sitter grammar available.
#ifdef MCPP_CODE_INTEL_TREESITTER
std::optional<std::vector<Symbol>> ts_extract(const std::string& source,
                                              const std::string& rel_path,
                                              const std::string& ext);
#endif

inline bool ts_engine_available() {
#ifdef MCPP_CODE_INTEL_TREESITTER
    return true;
#else
    return false;
#endif
}

struct IndexStats {
    size_t scanned = 0;      // files seen
    size_t rescanned = 0;    // files actually parsed (new or changed)
    size_t removed = 0;      // entries dropped (file deleted)
    size_t symbols = 0;
    long long ms = 0;
    std::string engine;      // "tree-sitter(+lexical)" | "lexical"
};

class CodeIndex {
public:
    void index(const std::string& root, IndexStats* stats = nullptr);

    // When enabled (host applications may turn this on), every query first
    // stats the tree and incrementally re-indexes anything that changed —
    // clients never need to call `index` twice.
    void set_auto_refresh(bool on) { auto_refresh_ = on; }
    bool auto_refresh() const { return auto_refresh_; }

    std::vector<Symbol> search(const std::string& query, const std::string& kind_filter,
                               size_t limit);
    std::vector<Symbol> symbols_in_file(const std::string& rel);
    std::vector<std::string> grep(const std::string& needle, bool case_sensitive,
                                  size_t limit);
    std::optional<std::string> read_file(const std::string& rel, int64_t start,
                                         int64_t end);
    // All case-sensitive word-boundary occurrences of `symbol` across cached
    // file contents, marked as definition when they coincide with a known
    // symbol of the same name.
    std::vector<Reference> references(const std::string& symbol, size_t limit);

    size_t file_count() const { return files_.size(); }
    size_t symbol_count() const { return symbols_.size(); }
    const std::string& root() const { return root_; }

private:
    struct FileEntry {
        std::string path;
        int64_t lines = 0;
        std::filesystem::file_time_type mtime{};
        std::vector<Symbol> symbols;
        std::string engine;  // "tree-sitter" | "lexical"
        std::string content;  // cached file body when <= kMaxCachedBytes
        static constexpr size_t kMaxCachedBytes = 4 * 1024 * 1024;
    };

    static std::string to_lower(std::string s) {
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    static bool is_skipped_dir(const std::filesystem::path& p) {
        static const std::unordered_set<std::string> kSkip = {
            ".git", "node_modules", "build", "dist", "target", "vendor",
            ".venv", "venv", "__pycache__", ".cache", "third_party", "deps"};
        for (const auto& part : p)
            if (kSkip.count(part.generic_string())) return true;
        return false;
    }

    static bool is_source(const std::filesystem::path& p) {
        static const std::unordered_set<std::string> kExt = {
            ".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hh", ".hxx",
            ".py", ".ts", ".tsx", ".js", ".jsx", ".mjs", ".rs", ".go",
            ".java", ".kt", ".swift", ".lua", ".rb"};
        return kExt.count(p.extension().generic_string()) > 0;
    }

    static int64_t file_line_count(const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        if (!in) return 0;
        int64_t n = 0;
        std::string line;
        while (std::getline(in, line)) ++n;
        return n;
    }

    bool is_within(const std::string& rel) const {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path rp = (fs::path(root_) / rel).lexically_normal();
        fs::path bs = fs::path(root_).lexically_normal();
        return rp.generic_string().rfind(bs.generic_string(), 0) == 0;
    }

    FileEntry scan_file(const std::filesystem::path& abs, const std::string& rel);
    void maybe_refresh();
    static std::vector<Symbol> lexical_scan(const std::string& content,
                                            const std::string& rel, const std::string& ext);

    std::string root_;
    std::map<std::string, FileEntry> files_;  // rel path -> entry
    std::vector<Symbol> symbols_;             // flattened, sorted for search
    bool auto_refresh_ = false;
    bool refreshing_ = false;
};

}  // namespace code_intel

// The implementation lives in code_index_impl.hpp so tests can include the
// declarations cheaply; main.cpp and tests both include it.
#include "code_index_impl.hpp"
