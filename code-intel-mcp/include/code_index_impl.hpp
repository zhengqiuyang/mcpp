// code_index_impl.hpp — implementation half of code_index.hpp.
#pragma once

namespace code_intel {

inline void CodeIndex::index(const std::string& root, IndexStats* stats) {
    namespace fs = std::filesystem;
    IndexStats st;
    root_ = root;

    std::unordered_set<std::string> seen;
    std::error_code ec;
    fs::path base = fs::absolute(fs::path(root), ec);
    for (fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        fs::path p = it->path();
        if (is_skipped_dir(p)) {
            it.disable_recursion_pending();
            continue;
        }
        if (!is_source(p)) continue;
        std::string rel = fs::relative(p, base, ec).generic_string();
        if (ec) continue;
        seen.insert(rel);
        ++st.scanned;

        auto mtime = fs::last_write_time(p, ec);
        if (ec) continue;
        auto old = files_.find(rel);
        if (old != files_.end() && old->second.mtime == mtime) continue;  // unchanged

        FileEntry entry = scan_file(p, rel);
        entry.mtime = mtime;
        files_[rel] = std::move(entry);
        ++st.rescanned;
    }

    // drop deleted files
    for (auto it = files_.begin(); it != files_.end();) {
        if (seen.count(it->first) == 0) {
            it = files_.erase(it);
            ++st.removed;
        } else {
            ++it;
        }
    }

    symbols_.clear();
    for (const auto& [rel, entry] : files_)
        symbols_.insert(symbols_.end(), entry.symbols.begin(), entry.symbols.end());
    std::sort(symbols_.begin(), symbols_.end(), [](const Symbol& a, const Symbol& b) {
        return a.file != b.file ? a.file < b.file : a.line < b.line;
    });
    st.symbols = symbols_.size();
    st.engine = ts_engine_available() ? "tree-sitter(+lexical)" : "lexical";
    if (stats) *stats = st;
}

inline CodeIndex::FileEntry CodeIndex::scan_file(const std::filesystem::path& abs,
                                                 const std::string& rel) {
    FileEntry entry;
    entry.path = rel;

    // read the whole body once; cache when reasonably small
    std::ifstream in(abs, std::ios::binary);
    if (!in) return entry;
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    entry.lines = 1;
    for (char c : content) entry.lines += (c == '\n');
    if (content.size() <= FileEntry::kMaxCachedBytes) entry.content = content;
    const std::string& body =
        entry.content.empty() ? content : entry.content;

    const std::string ext = abs.extension().generic_string();

#ifdef MCPP_CODE_INTEL_TREESITTER
    if (auto precise = ts_extract(body, rel, ext)) {
        entry.symbols = std::move(*precise);
        entry.engine = "tree-sitter";
    } else {
        entry.symbols = lexical_scan(body, rel, ext);
        entry.engine = "lexical";
    }
#else
    entry.symbols = lexical_scan(body, rel, ext);
    entry.engine = "lexical";
#endif
    for (auto& s : entry.symbols) s.name_lower = to_lower(s.name);
    return entry;
}

inline std::vector<Symbol> CodeIndex::search(const std::string& query,
                                             const std::string& kind_filter,
                                             size_t limit) const {
    std::string needle = to_lower(query);
    std::vector<Symbol> hits;
    for (const auto& s : symbols_) {
        if (!kind_filter.empty() && s.kind != kind_filter) continue;
        if (s.name_lower.find(needle) != std::string::npos) {
            hits.push_back(s);
            if (hits.size() >= limit) break;
        }
    }
    return hits;
}

inline std::vector<Symbol> CodeIndex::symbols_in_file(const std::string& rel) const {
    auto it = files_.find(rel);
    return it == files_.end() ? std::vector<Symbol>{} : it->second.symbols;
}

inline std::vector<std::string> CodeIndex::grep(const std::string& needle,
                                                bool case_sensitive,
                                                size_t limit) const {
    namespace fs = std::filesystem;
    std::vector<std::string> hits;
    std::string cmp_needle = case_sensitive ? needle : to_lower(needle);
    for (const auto& [rel, entry] : files_) {
        // cached content when available, else stream from disk
        if (!entry.content.empty()) {
            int64_t no = 0;
            size_t pos = 0;
            while (pos <= entry.content.size()) {
                size_t eol = entry.content.find('\n', pos);
                std::string line = entry.content.substr(
                    pos, eol == std::string::npos ? std::string::npos : eol - pos);
                ++no;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::string hay = case_sensitive ? line : to_lower(line);
                if (hay.find(cmp_needle) != std::string::npos) {
                    std::ostringstream os;
                    os << rel << ":" << no << ": " << line;
                    hits.push_back(os.str());
                    if (hits.size() >= limit) return hits;
                }
                if (eol == std::string::npos) break;
                pos = eol + 1;
            }
            continue;
        }
        std::ifstream in(fs::path(root_) / rel, std::ios::binary);
        if (!in) continue;
        std::string line;
        int64_t no = 0;
        while (std::getline(in, line)) {
            ++no;
            std::string hay = case_sensitive ? line : to_lower(line);
            if (hay.find(cmp_needle) != std::string::npos) {
                std::ostringstream os;
                os << rel << ":" << no << ": " << line;
                hits.push_back(os.str());
                if (hits.size() >= limit) return hits;
            }
        }
    }
    return hits;
}

inline std::vector<Reference> CodeIndex::references(const std::string& symbol,
                                                    size_t limit) const {
    std::vector<Reference> out;
    if (symbol.empty()) return out;
    auto ident = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    };
    for (const auto& [rel, entry] : files_) {
        // definitions of this symbol in this file (file,line)
        std::unordered_set<int64_t> def_lines;
        for (const auto& s : entry.symbols)
            if (s.name == symbol) def_lines.insert(s.line);

        const std::string* body = &entry.content;
        std::string disk_body;
        std::ifstream in;
        if (entry.content.empty()) {
            in.open(std::filesystem::path(root_) / rel, std::ios::binary);
            if (!in) continue;
            disk_body.assign((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
            body = &disk_body;
        }

        int64_t no = 0;
        size_t pos = 0;
        while (pos <= body->size() && out.size() < limit) {
            size_t eol = body->find('\n', pos);
            size_t len = eol == std::string::npos ? body->size() - pos : eol - pos;
            ++no;
            size_t at = body->find(symbol, pos);
            if (at < pos + len) {  // candidate occurrence within this line
                size_t after = at + symbol.size();
                bool boundary_before = at == pos || !ident((*body)[at - 1]);
                bool boundary_after = after >= pos + len || !ident((*body)[after]);
                if (boundary_before && boundary_after) {
                    std::string line = body->substr(pos, len);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    out.push_back({rel, no, def_lines.count(no) > 0, line.substr(0, 200)});
                }
            }
            if (eol == std::string::npos) break;
            pos = eol + 1;
        }
        if (out.size() >= limit) break;
    }
    return out;
}

inline std::optional<std::string> CodeIndex::read_file(const std::string& rel, int64_t start,
                                                       int64_t end) const {
    namespace fs = std::filesystem;
    if (!is_within(rel)) return std::nullopt;
    std::ifstream in(fs::path(root_) / rel, std::ios::binary);
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

// ---- lexical fallback scanner (ported from v0.1) ---------------------------

inline std::vector<Symbol> CodeIndex::lexical_scan(const std::string& content,
                                                   const std::string& rel,
                                                   const std::string& ext) {
    std::vector<Symbol> out;
    auto emit = [&](std::string name, std::string kind, int64_t no) {
        Symbol s;
        s.name = std::move(name);
        s.kind = std::move(kind);
        s.file = rel;
        s.line = no;
        out.push_back(std::move(s));
    };

    std::string line;
    int64_t no = 0;
    size_t cursor = 0;
    auto next_line = [&]() -> bool {
        if (cursor > content.size()) return false;
        size_t eol = content.find('\n', cursor);
        line = content.substr(cursor, eol == std::string::npos ? std::string::npos
                                                               : eol - cursor);
        cursor = eol == std::string::npos ? content.size() + 1 : eol + 1;
        ++no;
        return true;
    };
    while (next_line()) {
        ++no;
        const std::string t = [&] {
            size_t b = line.find_first_not_of(" \t");
            size_t e = line.find_last_not_of(" \t\r\n");
            return b == std::string::npos ? "" : line.substr(b, e - b + 1);
        }();
        if (t.empty() || t[0] == '#') continue;

        auto word_after = [&](size_t pos) {
            size_t s = t.find_first_not_of(" \t", pos);
            if (s == std::string::npos) return std::string();
            size_t e = s;
            while (e < t.size() && (std::isalnum(static_cast<unsigned char>(t[e])) || t[e] == '_')) ++e;
            return t.substr(s, e - s);
        };
        auto ident_char = [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        };

        if (ext == ".py") {
            const size_t indent = line.find_first_not_of(" \t");
            if (t.rfind("def ", 0) == 0) {
                auto name = word_after(3);
                if (!name.empty()) emit(name, indent == 0 ? "function" : "method", no);
                continue;
            }
            if (t.rfind("class ", 0) == 0) {
                auto name = word_after(5);
                if (!name.empty()) emit(name, "class", no);
                continue;
            }
        }

        if (ext == ".js" || ext == ".jsx" || ext == ".mjs" || ext == ".ts" || ext == ".tsx") {
            size_t pos = t.find("function ");
            if (pos != std::string::npos) {
                auto name = word_after(pos + 8);
                if (!name.empty()) { emit(name, "function", no); continue; }
            }
            pos = t.find("class ");
            if (pos != std::string::npos) {
                auto name = word_after(pos + 5);
                if (!name.empty()) { emit(name, "class", no); continue; }
            }
            size_t c = t.find("const ");
            if (c != std::string::npos && t.find("=>") != std::string::npos) {
                auto name = word_after(c + 5);
                if (!name.empty()) emit(name, "definition", no);
            }
            continue;
        }

        if (ext == ".rs" || ext == ".go" || ext == ".java" || ext == ".kt" || ext == ".swift") {
            for (const char* kw : {"fn ", "func ", "def "}) {
                size_t pos = t.find(kw);
                if (pos != std::string::npos) {
                    auto name = word_after(pos + std::strlen(kw));
                    if (!name.empty()) { emit(name, "function", no); goto next_line; }
                }
            }
            {
                static const std::unordered_set<std::string> kControl = {
                    "if", "for", "while", "switch", "catch", "return", "match", "guard"};
                size_t p = t.find('(');
                while (p != std::string::npos && p > 0) {
                    size_t e = p;
                    while (e > 0 && ident_char(t[e - 1])) --e;
                    std::string name = t.substr(e, p - e);
                    if (!name.empty() && !kControl.count(name)) { emit(name, "function", no); break; }
                    p = t.find('(', p + 1);
                }
            }
            continue;
        }

        // C family
        {
            for (const char* kw : {"class ", "struct ", "enum "}) {
                size_t pos = t.find(kw);
                if (pos != std::string::npos) {
                    auto name = word_after(pos + std::strlen(kw));
                    if (!name.empty()) {
                        std::string kind(kw);
                        kind.pop_back();
                        emit(name, kind, no);
                        goto next_line;
                    }
                }
            }
            static const std::unordered_set<std::string> kControl = {
                "if", "for", "while", "switch", "catch", "return", "sizeof", "defined"};
            size_t p = t.find('(');
            if (p == std::string::npos || p == 0) continue;
            size_t e = p;
            while (e > 0 && ident_char(t[e - 1])) --e;
            std::string name = t.substr(e, p - e);
            if (name.empty() || kControl.count(name)) continue;
            if (t.find('{') != std::string::npos || t.find("const") != std::string::npos ||
                t.back() == ')')
                emit(name, "function", no);
            continue;
        }
    next_line:;
    }
    return out;
}

}  // namespace code_intel
