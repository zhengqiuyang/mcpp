# Vendored dependencies (tree-sitter)

Vendored so `CODE_INTEL_TREESITTER=ON` builds work offline and reproducibly.
All upstreams are MIT-licensed (see the LICENSE in each directory).

| Directory | Upstream | Pinned commit |
|---|---|---|
| `tree-sitter/` | github.com/tree-sitter/tree-sitter (runtime, lib/src amalgam + api.h) | fbfb07e |
| `tree-sitter-c/` | github.com/tree-sitter/tree-sitter-c | b780e47 |
| `tree-sitter-cpp/` | github.com/tree-sitter/tree-sitter-cpp | c009222 |
| `tree-sitter-python/` | github.com/tree-sitter/tree-sitter-python | 26855ea |
| `tree-sitter-typescript/` | github.com/tree-sitter/tree-sitter-typescript (typescript + tsx) | 75b3874 |

Only the generated parser/scanner sources needed at build time are kept
(no grammar.js, bindings, docs). Update by copying the same file set from a
new upstream release and bumping the commit above.
