# code-intel-mcp

高速代码库智能 MCP server——给 Claude Code / Cline / Cursor 提供**本地、常驻、
毫秒级**的符号搜索 / grep / 分页读文件。构建在 [mcpp](../mcpp) 之上,
是 SDK 的第一个自用杀手级应用。

## 快速开始

```bash
cmake -S code-intel-mcp -B code-intel-mcp/build && cmake --build code-intel-mcp/build -j
# 或零构建系统直编:
g++ -std=c++17 -I ../mcpp/include src/main.cpp -o code-intel-mcp
```

接进 Claude Code(`claude_code_config.json`):

```json
{
  "mcpServers": {
    "code-intel": { "command": "/path/to/code-intel-mcp", "args": [] }
  }
}
```

对 agent 说:"先调用 index 索引仓库,然后用 search_symbols 找 McpServer 的定义"。

## 工具面

| 工具 | 说明 |
|---|---|
| `index {root}` | 索引源码树(跳过 .git/node_modules/build/vendor;C/C++/Python/TS/JS/Rust/Go/Java/Kotlin/Swift/Lua/Ruby) |
| `search_symbols {query, kind?, limit?}` | 符号名子串搜索 |
| `list_symbols {path}` | 单文件符号清单 |
| `grep {pattern, case_sensitive?, limit?}` | 跨文件字面量搜索 |
| `read_file {path, start_line?, end_line?}` | 带行号分页读文件 |

实测(mcpp 源码,12 文件 / ~2600 行):索引 5ms,查询亚毫秒。

## 设计文档

[docs/design.md](docs/design.md) — v0.1 词法扫描 → v0.2 tree-sitter 精确解析 →
v0.3 符号引用图 + BM25 → v0.4 本地 embedding 语义搜索,及基准门槛。

License:MIT。
