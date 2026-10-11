// test_index.cpp — CodeIndex unit tests: precise symbols (tree-sitter when
// enabled, lexical otherwise) and incremental re-indexing.
#include "code_index.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using code_intel::CodeIndex;
using code_intel::IndexStats;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

static bool has_symbol(const std::vector<code_intel::Symbol>& syms, const std::string& name,
                       const std::string& kind) {
    for (const auto& s : syms)
        if (s.name == name && s.kind == kind) return true;
    return false;
}

static void write(const fs::path& p, const std::string& content) {
    std::ofstream(p, std::ios::binary) << content;
}

int main() {
    fs::path dir = fs::temp_directory_path() / ("code-intel-test-" + std::to_string(::rand()));
    fs::create_directories(dir / "src");

    write(dir / "src" / "sample.cpp",
          "#include <string>\n"
          "class Widget {\n"
          "public:\n"
          "    void spin(int n) { (void)n; }\n"
          "    int value() const { return 42; }\n"
          "};\n"
          "\n"
          "static Widget make_widget(const std::string& name) { (void)name; return Widget{}; }\n"
          "\n"
          "int main() { return 0; }\n");
    write(dir / "src" / "app.py",
          "def top_level(x):\n"
          "    return x\n"
          "\n"
          "class Greeter:\n"
          "    def greet(self):\n"
          "        return 'hi'\n");
    write(dir / "src" / "util.ts",
          "export function format(n: number): string { return String(n); }\n"
          "class Store { get() { return 1; } }\n"
          "const handler = (x: number) => x * 2;\n");

    CodeIndex index;
    IndexStats st;
    index.index(dir.generic_string(), &st);
    CHECK(st.scanned == 3);

    auto cpp_syms = index.symbols_in_file("src/sample.cpp");
#ifdef MCPP_CODE_INTEL_TREESITTER
    CHECK(has_symbol(cpp_syms, "Widget", "class"));
    CHECK(has_symbol(cpp_syms, "spin", "method"));
    CHECK(has_symbol(cpp_syms, "value", "method"));
    CHECK(has_symbol(cpp_syms, "make_widget", "function"));
    // prototypes/calls must NOT be symbols (precision)
    CHECK(!has_symbol(cpp_syms, "string", "class"));

    auto py_syms = index.symbols_in_file("src/app.py");
    CHECK(has_symbol(py_syms, "top_level", "function"));
    CHECK(has_symbol(py_syms, "Greeter", "class"));
    CHECK(has_symbol(py_syms, "greet", "method"));

    auto ts_syms = index.symbols_in_file("src/util.ts");
    CHECK(has_symbol(ts_syms, "format", "function"));
    CHECK(has_symbol(ts_syms, "Store", "class"));
    CHECK(has_symbol(ts_syms, "get", "method"));
    CHECK(has_symbol(ts_syms, "handler", "definition"));
#else
    // lexical fallback: coarser guarantees, just sanity-check coverage
    CHECK(!cpp_syms.empty());
    CHECK(has_symbol(cpp_syms, "Widget", "class"));
#endif

    // search across files
    auto hits = index.search("widget", "", 10);
    CHECK(!hits.empty());
    auto exact = index.search("Greeter", "class", 10);
    CHECK(exact.size() == 1 && exact[0].file == "src/app.py");

    // ---- incremental re-index: no changes -> nothing rescanned ------------
    IndexStats st2;
    index.index(dir.generic_string(), &st2);
    CHECK(st2.rescanned == 0);
    CHECK(st2.removed == 0);

    // ---- one file changed -> only that file rescanned ---------------------
    write(dir / "src" / "app.py",
          "def renamed_entry():\n"
          "    pass\n");
    // force an mtime the index cannot have seen (FS timestamp granularity
    // varies across platforms)
    fs::last_write_time(dir / "src" / "app.py",
                        fs::file_time_type::clock::now() + std::chrono::seconds(1));
    IndexStats st3;
    index.index(dir.generic_string(), &st3);
    CHECK(st3.rescanned == 1);
    CHECK(index.symbols_in_file("src/app.py").size() == 1);
    CHECK(has_symbol(index.symbols_in_file("src/app.py"), "renamed_entry", "function"));
    // other files kept their symbols without rescanning
    CHECK(has_symbol(index.symbols_in_file("src/sample.cpp"), "Widget", "class"));

    // ---- deletion -> removed ------------------------------------------------
    fs::remove(dir / "src" / "util.ts");
    IndexStats st4;
    index.index(dir.generic_string(), &st4);
    CHECK(st4.removed == 1);
    CHECK(index.symbols_in_file("src/util.ts").empty());
    CHECK(index.file_count() == 2);

    // ---- grep + read_file ---------------------------------------------------
    auto lines = index.grep("make_widget", true, 5);
    CHECK(lines.size() == 1 && lines[0].find("src/sample.cpp:8") == 0);
    auto body = index.read_file("src/sample.cpp", 2, 3);
    CHECK(body.has_value());
    CHECK(body->find("class Widget") != std::string::npos);
    CHECK(index.read_file("../outside", 1, 10) == std::nullopt);  // sandboxed

    fs::remove_all(dir);
    std::printf("test_index: %d checks, %d failures (engine: %s)\n", g_checks, g_failures,
                code_intel::ts_engine_available() ? "tree-sitter" : "lexical");
    return g_failures == 0 ? 0 : 1;
}
