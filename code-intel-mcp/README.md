# code-intel-mcp

高速代码库智能 MCP server——给 Claude Code / Cline / Cursor 提供**本地、常驻、
毫秒级**的符号搜索 / grep / 分页读文件。构建在 [mcpp](../mcpp) 之上,
是 SDK 的第一个自用杀手级应用。

## 引擎与精度(v0.2)

两个提取引擎,**tree-sitter 为默认**(vendored,离线可构建,MIT):

- **tree-sitter**(C/C++/Python/TypeScript/TSX):精确的函数/方法/类/结构体/
  枚举/箭头函数定义——包括构造函数、析构函数、`operator=` 等词法版抓不到的符号,
  且零函数调用误报。实测 mcpp 源码树:21 文件、248 符号、36ms。
- **词法启发式**(其余语言回退):`cmake -DCODE_INTEL_TREESITTER=OFF` 时为唯一引擎,
  保持零依赖构建。

**增量与自动化(v0.3)**:mtime 未变的文件直接复用;**并行扫描**(多核,实测
26 文件 323 符号 17ms);**查询时自动刷新**——每条查询先 stat 一遍目录树,
有变更即增量重扫,agent 不需要反复调用 index。

## 快速开始

```bash
cmake -S code-intel-mcp -B code-intel-mcp/build && cmake --build code-intel-mcp/build -j
ctest --test-dir code-intel-mcp/build --output-on-failure   # 28 checks
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
| `references {symbol, limit?}` | 全词匹配找引用,定义标 `[def]`、使用标 `[use]`(内容缓存加速) |
| `read_file {path, start_line?, end_line?}` | 带行号分页读文件 |

实测(mcpp 源码,21 文件):索引 36ms(tree-sitter),查询亚毫秒;重索引仅扫
变更文件。

## 设计文档

[docs/design.md](docs/design.md) — v0.1 词法扫描 → v0.2 tree-sitter 精确解析 →
v0.3 符号引用图 + BM25 → v0.4 本地 embedding 语义搜索,及基准门槛。

License:MIT。
