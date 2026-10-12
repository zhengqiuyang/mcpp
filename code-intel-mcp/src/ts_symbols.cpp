// ts_symbols.cpp — precise symbol extraction via vendored tree-sitter
// grammars (C / C++ / Python / TypeScript / TSX). Compiled only when the
// build enables MCPP_CODE_INTEL_TREESITTER.
#ifdef MCPP_CODE_INTEL_TREESITTER

#include "code_index.hpp"

#include <cctype>
#include <cstring>

#include <tree_sitter/api.h>

extern "C" {
const TSLanguage* tree_sitter_c(void);
const TSLanguage* tree_sitter_cpp(void);
const TSLanguage* tree_sitter_python(void);
const TSLanguage* tree_sitter_typescript(void);
const TSLanguage* tree_sitter_tsx(void);
}

namespace code_intel {
namespace {

// tree-sitter's field lookup takes an explicit name length.
inline TSNode field(TSNode n, const char* name) {
    return ts_node_child_by_field_name(n, name, static_cast<uint32_t>(std::strlen(name)));
}

const TSLanguage* lang_for(const std::string& ext) {
    if (ext == ".c") return tree_sitter_c();
    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".hpp" || ext == ".hh" ||
        ext == ".hxx" || ext == ".h")  // cpp grammar is a superset; parses C headers
        return tree_sitter_cpp();
    if (ext == ".py") return tree_sitter_python();
    if (ext == ".ts" || ext == ".mts" || ext == ".cts") return tree_sitter_typescript();
    if (ext == ".tsx" || ext == ".jsx") return tree_sitter_tsx();
    return nullptr;
}

struct Walk {
    const char* src;
    size_t src_len;
    std::vector<Symbol> out;

    std::string text_of(TSNode n) const {
        uint32_t s = ts_node_start_byte(n), e = ts_node_end_byte(n);
        if (s >= src_len || e > src_len || e < s) return "";
        return std::string(src + s, e - s);
    }

    static std::string type_of(TSNode n) { return ts_node_type(n); }

    // C-family: function_definition -> declarator (possibly nested through
    // pointer/parenthesized declarators) -> identifier.
    std::string c_function_name(TSNode def) const {
        TSNode decl = field(def, "declarator");
        int guard = 0;
        while (!ts_node_is_null(decl) && guard++ < 8) {
            TSNode inner = field(decl, "declarator");
            if (ts_node_is_null(inner)) break;
            decl = inner;
        }
        return ts_node_is_null(decl) ? "" : pure_identifier(text_of(decl));
    }

    // Reduce declarator text to a plain identifier: drop surrounding
    // qualifiers/whitespace ("& operator=(const X&)" -> "operator=") while
    // keeping destructor tildes.
    static std::string pure_identifier(const std::string& t) {
        std::string out;
        for (char c : t)
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') out.push_back(c);
        size_t p = out.find('(');
        if (p != std::string::npos) out.resize(p);
        size_t s = 0;
        while (s < out.size() &&
               !(std::isalnum(static_cast<unsigned char>(out[s])) || out[s] == '_' ||
                 (out[s] == '~' && s == 0)))
            ++s;
        return out.substr(s);
    }

    bool inside_class_body(TSNode n) const {
        for (int i = 0; i < 6; ++i) {
            n = ts_node_parent(n);
            if (ts_node_is_null(n)) return false;
            const std::string t = type_of(n);
            if (t == "class_specifier" || t == "class_definition" ||
                t == "struct_specifier")
                return true;
        }
        return false;
    }

    void emit(std::string name, std::string kind, TSNode at) {
        if (name.empty() || name.size() > 256) return;
        TSPoint p = ts_node_start_point(at);
        Symbol s;
        s.name = std::move(name);
        s.kind = std::move(kind);
        s.file = "";
        s.line = static_cast<int64_t>(p.row) + 1;
        out.push_back(std::move(s));
    }

    void visit(TSNode n, int depth) {
        if (depth > 400 || out.size() > 50000) return;
        const std::string t = type_of(n);
        const bool is_cxx = cxx_mode;

        if (is_cxx) {
            if (t == "function_definition") {
                std::string name = c_function_name(n);
                emit(std::move(name), inside_class_body(n) ? "method" : "function", n);
            } else if (t == "class_specifier" || t == "struct_specifier" ||
                       t == "enum_specifier") {
                TSNode nm = field(n, "name");
                if (!ts_node_is_null(nm))
                    emit(text_of(nm),
                         t == "class_specifier" ? "class"
                                                : (t == "struct_specifier" ? "struct" : "enum"),
                         n);
            }
        } else if (python_mode) {
            if (t == "function_definition") {
                TSNode nm = field(n, "name");
                if (!ts_node_is_null(nm))
                    emit(text_of(nm), inside_class_body(n) ? "method" : "function", n);
            } else if (t == "class_definition") {
                TSNode nm = field(n, "name");
                if (!ts_node_is_null(nm)) emit(text_of(nm), "class", n);
            }
        } else {  // TypeScript / TSX
            if (t == "function_declaration" || t == "generator_function_declaration") {
                TSNode nm = field(n, "name");
                if (!ts_node_is_null(nm)) emit(text_of(nm), "function", n);
            } else if (t == "method_definition") {
                TSNode name_node = field(n, "name");
                if (!ts_node_is_null(name_node)) emit(text_of(name_node), "method", n);
            } else if (t == "class_declaration" || t == "abstract_class_declaration") {
                TSNode nm = field(n, "name");
                if (!ts_node_is_null(nm)) emit(text_of(nm), "class", n);
            } else if (t == "lexical_declaration") {
                // const fn = (...) => ...  → definition. The TS grammar does
                // not label the declarator field, so search children by type.
                uint32_t cnt = ts_node_child_count(n);
                for (uint32_t i = 0; i < cnt; ++i) {
                    TSNode v = ts_node_child(n, i);
                    if (type_of(v) != "variable_declarator") continue;
                    TSNode nm = field(v, "name");
                    if (ts_node_is_null(nm)) continue;
                    bool fn_like = false;
                    TSNode val = field(v, "value");
                    if (!ts_node_is_null(val) &&
                        type_of(val).find("function") != std::string::npos)
                        fn_like = true;
                    if (!fn_like) {
                        uint32_t vc = ts_node_child_count(v);
                        for (uint32_t j = 0; j < vc; ++j)
                            if (type_of(ts_node_child(v, j)).find("function") !=
                                std::string::npos)
                                fn_like = true;
                    }
                    if (fn_like) emit(text_of(nm), "definition", n);
                }
            }
        }

        uint32_t cnt = ts_node_child_count(n);
        for (uint32_t i = 0; i < cnt; ++i) visit(ts_node_child(n, i), depth + 1);
    }

    bool cxx_mode = false;
    bool python_mode = false;
};

}  // namespace

std::optional<std::vector<Symbol>> ts_extract(const std::string& source,
                                              const std::string& rel_path,
                                              const std::string& ext) {
    const TSLanguage* lang = lang_for(ext);
    if (!lang) return std::nullopt;

    TSParser* parser = ts_parser_new();
    if (!ts_parser_set_language(parser, lang)) {
        ts_parser_delete(parser);
        return std::nullopt;
    }
    TSTree* tree = ts_parser_parse_string(parser, nullptr, source.data(),
                                          static_cast<uint32_t>(source.size()));
    if (!tree) {
        ts_parser_delete(parser);
        return std::nullopt;
    }

    Walk walk;
    walk.src = source.data();
    walk.src_len = source.size();
    walk.python_mode = ext == ".py";
    walk.cxx_mode = !walk.python_mode &&
                    (ext == ".c" || ext == ".cpp" || ext == ".cc" || ext == ".cxx" ||
                     ext == ".hpp" || ext == ".hh" || ext == ".hxx" || ext == ".h");

    TSNode root = ts_tree_root_node(tree);
    // Visit only named children chains? Full walk is fine for grammars.
    walk.visit(root, 0);

    ts_tree_delete(tree);
    ts_parser_delete(parser);

    for (auto& s : walk.out) s.file = rel_path;
    return walk.out;
}

}  // namespace code_intel

#endif  // MCPP_CODE_INTEL_TREESITTER
