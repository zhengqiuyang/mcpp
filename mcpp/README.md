# mcpp — a zero-dependency C++17 SDK for the Model Context Protocol

`mcpp` lets any C++ application **natively speak MCP** — expose tools,
resources and prompts to AI agents (Claude Desktop/Code, Cursor, Cline, …),
or act as an MCP client — with **no external dependencies, no Python bridge,
no runtime baggage**. One header directory, C++17, links into anything.

```cpp
#include "mcpp/server.hpp"

int main() {
    mcpp::McpServer server("com.zqy.demo", "1.0.0");
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

Point Claude Desktop / MCP Inspector at the built binary and the `add` tool
is immediately callable by the model.

## Why

MCP has official SDKs for TypeScript, Python, Java, Kotlin, C#, Ruby, Swift,
PHP, Go and Rust — **but not C++**. The world's game engines, robot stacks,
industrial software, trading systems and embedded devices are C++; today the
only way to give them an MCP surface is bolting on a Python sidecar.
`mcpp` fills that gap natively.

Design principles:

- **Zero dependencies** — bundled `json.hpp` (exact int64 round-trip, precise
  error offsets, deterministic output), no exceptions on hot paths.
- **Embeddable first** — header-only, synchronous core, no threads required;
  bring your own event loop via the `Transport` interface.
- **Spec-shaped** — stable MCP core (initialize handshake, tools/resources/
  prompts, stdio framing, JSON-RPC 2.0 error envelopes) with a clear
  compliance table. No magic, no hidden I/O.

## Layout

| Path | Contents |
|---|---|
| `include/mcpp/json.hpp` | Self-contained JSON value/parser/serializer |
| `include/mcpp/protocol.hpp` | Wire types, content blocks, JSON-RPC message helpers |
| `include/mcpp/transport.hpp` | `Transport` interface + stdio + in-memory pair |
| `include/mcpp/server.hpp` | `McpServer`: registration API + dispatch loop |
| `include/mcpp/client.hpp` | `McpClient`: initialize / list_tools / call_tool |
| `examples/` | echo, sandboxed filesystem, sysinfo servers + `mini_client` (spawns any MCP server binary and talks to it) |
| `tests/` | JSON unit tests + full client↔server protocol tests |

## Build

```bash
cmake -S mcpp -B mcpp/build
cmake --build mcpp/build -j
ctest --test-dir mcpp/build --output-on-failure
```

Or without CMake — it's just headers:

```bash
g++ -std=c++17 -I mcpp/include examples/echo_server.cpp -o echo_server
```

Drop `include/` into any project (`#include "mcpp/server.hpp"`), or vendor
the four headers — there is nothing else to install.

## Client side

```cpp
mcpp::ProcessTransport transport({"./my_server"});   // see examples/mini_client.cpp
mcpp::McpClient client(transport);
auto info = client.initialize("my-app", "1.0");
for (const auto& tool : client.list_tools()) { /* ... */ }
auto r = client.call_tool("add", mcpp::Json{{"a", 19}, {"b", 23}});
// r.text_content() == "42"; r.is_error == false
```

## Compliance status

| Area | Status |
|---|---|
| JSON-RPC 2.0 (requests/notifications/responses/error envelope) | ✅ |
| `initialize` handshake + version negotiation (2024-11-05 / 2025-03-26 / 2025-06-18) | ✅ |
| Tools (`tools/list`, `tools/call`, input schemas, `isError`) | ✅ |
| Resources (`resources/list`, `resources/read`, text bodies) | ✅ |
| Prompts (`prompts/list`, `prompts/get`, arguments) | ✅ |
| `ping` | ✅ |
| stdio transport (newline-delimited JSON) | ✅ |
| Server-not-initialized gating (-32002) | ✅ |
| Streamable HTTP transport | 🚧 roadmap |
| `notifications/*` progress, cancellation propagation | 🚧 (cancel is acknowledged, sync handlers can't abort) |
| sampling / roots / elicitation (client-side agent features) | 🚧 roadmap |
| OAuth for remote transports | 📋 backlog |

## Roadmap

1. **v0.1** — this release: stdio core, full tests, examples, CI matrix
   (gcc/clang/msvc × linux/macos/windows).
2. **v0.2** — `ProcessTransport` graduated into the library; Streamable HTTP
   transport; compliance runs against the official MCP Inspector in CI.
3. **v0.3** — completion API (notifications/progress, cancellation hooks),
   benchmark suite vs. the TypeScript SDK, vcpkg + conan recipes.

## Relation to existing C++ MCP libraries

Community libraries exist (cpp-mcp, TinyMCP, mcp-cpp, IBM TSAR-MCP) — all
early, none a de facto standard. `mcpp` differs by shipping a **dependency-
free embeddable core with a synchronous, game-loop-friendly design** and a
published compliance table. We'd rather converge than compete: if one library
wins, porting cost here is low because the API surface is small.

## License

MIT — see [LICENSE](LICENSE).
