# design: code-intel-mcp

高速代码库智能 MCP server——给 Claude Code / Cline / Cursor 等 coding agent
提供"本地、常驻、毫秒级"的代码检索层。对标 Serena(Python/LSP,冷启动慢)、
Sourcegraph(商业)。这是 mcpp 的第一个自用杀手级应用。

## 为什么

- Coding agent 的最大瓶颈之一是上下文获取:每次都要重新 grep/读文件。
  一个常驻内存的原生索引能把"找符号"从秒级降到毫秒级。
- 现有方案是 Python/LSP 或 Rust;没有 C++ 生态的原生实现,mcpp 正好缺
  一个"用自己 SDK 写的、有真实用户"的应用来证明。

## 工具面(v0.1 已实现)

| 工具 | 说明 |
|---|---|
| `index {root}` | 递归索引源码树,跳过 .git/node_modules/build/vendor |
| `search_symbols {query, kind?, limit?}` | 符号名子串搜索(C++/Python/TS/JS/Rust/Go/Java…) |
| `list_symbols {path}` | 单文件符号清单 |
| `grep {pattern, case_sensitive?, limit?}` | 跨文件字面量搜索 |
| `read_file {path, start_line?, end_line?}` | 带行号分页读文件 |

资源:`intel://stats`(索引统计)。

## v0.1 符号提取:词法启发式(已实现)

按语言分派的单行启发式(Python def/class、TS function/class/arrow、
C 家族 identifier( ) { 形状匹配)。精度 ~90%,实现 <300 行、零依赖。
误报率可接受——搜索场景宁多勿漏。

## v0.2:tree-sitter 精确解析(规划)

- Vendor tree-sitter runtime + C++/Python/TypeScript 三个 grammar(单独
  CMake 选项 `CODE_INTEL_TREESITTER=ON`,保持零依赖构建不受影响)
- 产出:精确的函数/类/方法 + 签名 + 纯虚信息;文件级增量解析
- 查询:tree-sitter query 暴露为 `ts_query {path, query}` 工具(高级用户)

## v0.3:符号图与混合检索

- 引用图:定义处落点 + 逐文件 use 遍历(build call graph / find references)
- BM25 词频检索(manuscript: 自实现 ~200 行,保持零依赖)
- 融合排序:符号精确命中 > BM25 > grep

## v0.4:语义层(可选)

- 本地 embedding(ONNX Runtime + all-MiniLM)做语义搜索
- 混合排序:BM25 + 向量(usearch 单文件索引)

## 基准目标(发布门槛)

| 指标 | 目标 |
|---|---|
| 索引 mcpp+code-intel 源码(~4k 行) | < 100 ms |
| 索引 1M 行 C++ 项目 | < 60 s(词法)/ < 5 min(tree-sitter 增量后重索引 < 1s/文件) |
| search_symbols P99 | < 1 ms |
| 常驻内存 | < 100 MB(1M 行,符号 + BM25) |

## 配置示例(Claude Code)

```json
{
  "mcpServers": {
    "code-intel": {
      "command": "/path/to/code-intel-mcp",
      "args": [],
      "env": {"CODE_INTEL_ROOT": "/path/to/repo"}
    }
  }
}
```
