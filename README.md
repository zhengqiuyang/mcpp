# mcpp — the C++ SDK for the Model Context Protocol

[![CI](https://github.com/zhengqiuyang/mcpp/actions/workflows/ci.yml/badge.svg)](https://github.com/zhengqiuyang/mcpp/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Release](https://img.shields.io/badge/release-v0.1.0-green.svg)](https://github.com/zhengqiuyang/mcpp/releases)

MCP has official SDKs for TypeScript, Python, Java, Kotlin, C#, Ruby, Swift,
PHP, Go and Rust — **but not C++**. Game engines, robot stacks, industrial
software, trading systems and embedded devices are C++, and today the only
way to give them an MCP surface is bolting on a Python sidecar.

**`mcpp` fills that gap natively**: a zero-dependency, header-only C++17 SDK
that lets any C++ application expose tools/resources/prompts to AI agents
(Claude Desktop/Code, Cursor, Cline, …) or act as an MCP client.

```cpp
#include "mcpp/server.hpp"

int main() {
    mcpp::McpServer server("com.example.demo", "1.0.0");
    server.add_tool("add", "Add two integers",
                    {{"a", "integer", "left"}, {"b", "integer", "right"}},
                    [](const mcpp::Json& args) {
                        return mcpp::ToolResult::text(
                            std::to_string(args.get("a").as_int() + args.get("b").as_int()));
                    });
    mcpp::StdioTransport transport;   // newline-delimited JSON on stdin/stdout
    server.run(transport);
}
```

Point Claude Desktop or the MCP Inspector at the binary — the tool is live.

## Why it's credible

- **9.2× faster** full round-trips than the official TypeScript SDK in an
  apples-to-apples benchmark (228k vs 24.8k ops/s, single thread) — see
  [`benchmarks/ts-sdk/`](benchmarks/ts-sdk/)
- **Validated by the official MCP Inspector CLI** (initialize / tools/list /
  tools/call) and by **[mcp-test](https://github.com/zhengqiuyang/mcp-test)**,
  the author's MCP security tool: 6/6 functional tests, 16/16 injection
  probes, **0 leaks** — wired in as a CI job
- **Zero dependencies, zero runtime**: four headers, C++17, no threads
  required, embeds into any event loop via the `Transport` interface

## Quick start

```bash
git clone https://github.com/zhengqiuyang/mcpp && cd mcpp
cmake -S mcpp -B mcpp/build && cmake --build mcpp/build -j
ctest --test-dir mcpp/build --output-on-failure     # 5/5 green

# drive it with the official inspector
npx @modelcontextprotocol/inspector mcpp/build/echo_server
```

Or without CMake — it's just headers:

```bash
g++ -std=c++17 -I mcpp/include mcpp/examples/echo_server.cpp -o echo_server
```

Full API, compliance table and benchmarks: **[`mcpp/README.md`](mcpp/README.md)**

## What's in this repo

| Path | What |
|---|---|
| [`mcpp/`](mcpp/) | The SDK: JSON / protocol / stdio+process+HTTP transports / server / client, tests, examples, CI |
| [`code-intel-mcp/`](code-intel-mcp/) | First app built on mcpp: fast codebase indexing for Claude Code / Cline (5ms to index mcpp itself) |
| [`docs/`](docs/) | Roadmap (10-week plan + stop-loss signals) and sub-project designs (ROS2 bridge, edge agent runtime, offline meeting notes) |
| [`benchmarks/`](benchmarks/) | Cross-SDK comparison harnesses |

中文版项目背景与整体规划:[`docs/program-zh.md`](docs/program-zh.md)

## Status & roadmap

v0.1 ships the stable MCP core: initialize handshake with version
negotiation, tools/resources/prompts, stdio + process + Streamable HTTP
(POST-only) transports on both server and client sides, JSON-RPC error
envelopes, and a published compliance table. Next: SSE GET stream,
`Mcp-Session-Id`, tree-sitter code-intel, vcpkg/conan recipes —
see [`docs/ROADMAP.md`](docs/ROADMAP.md).

Community libraries exist (cpp-mcp, TinyMCP, mcp-cpp, IBM TSAR-MCP) — all
early, none a de facto standard. mcpp differs by shipping a dependency-free
embeddable core with a synchronous, game-loop-friendly design. We'd rather
converge than compete.

## License

MIT — see [LICENSE](LICENSE).
