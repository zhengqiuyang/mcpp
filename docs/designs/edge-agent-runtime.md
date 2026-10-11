# design: edge-agent-runtime(单二进制 agent 运行时)

"agent 版的 llama.cpp":一个静态链接、<50MB 常驻内存的 C++ agent runtime,
把 tool-call 循环、结构化输出、子进程沙箱做成库 + CLI。Python/TS 统治 agent
框架的今天,端侧(车载/工控/机器人/边缘盒子)跑不动 Python——这是结构性空位,
且直接对齐 NVIDIA "inference + agent orchestration in C++" 招聘方向。

## 定位

```
┌─────────────── edge-agent(单二进制)───────────────┐
│ agent loop:reason → tool_call → observe → repeat    │
│ model backend(可插拔):                              │
│   · 内嵌 llama.cpp(完全离线)                        │
│   · llama-server / OpenAI 兼容 HTTP(本地服务)       │
│   · 云 API(降级模式)                                │
│ tools:进程沙箱执行器(seccomp / Windows Job Objects) │
│        + MCP client(经 mcpp:接入整个 MCP 生态)     │
│ structured output:GBNF / JSON-schema 约束            │
└──────────────────────────────────────────────────────┘
```

## 差异化

| 维度 | Python/TS 框架 | edge-agent |
|---|---|---|
| 部署 | 解释器 + 依赖树 | 单文件 scp 即用 |
| 冷启动 | 秒级 | <50ms |
| 内存 | 100MB+ | <50MB |
| 沙箱 | 语言层(可逃逸) | OS 层(进程/系统调用) |
| MCP | 生态原生 | 经 mcpp 同样原生 |

## v0.1 范围(10-12 周,启动条件见 ROADMAP)

1. 核心 agent loop + 三种 model backend 中先做 llama-server HTTP
2. 工具:shell 执行(带超时/工作目录隔离)+ 文件读写
3. MCP client 桥(任意 MCP server 的工具直接进 agent 工具箱)
4. JSON-schema 结构化输出(先靠 llama-server 的语法约束)
5. CLI:`edge-agent --task "..." --tools fs,shell,mcp`

砍掉:多 agent 编排、内存持久化规划、GUI。

## 风险

- llama.cpp 生态内已有人做类似(关注其 examples/agent 演进)
- "够用的 Python"在设备放得下时,性能卖点弱化 → 聚焦车规/工控
  (不能装 Python 的封闭系统)
