# cpp-agent-lab

C++ × AI Agent 基础设施项目群。核心判断:**推理层已被 llama.cpp 饱和,"agent 工具链 + 检索基础设施"是 C++ 的系统性空白,而 MCP 官方 SDK 覆盖十种语言唯独没有 C++**——本仓库群就是填这个缺口的。

```
cpp-agent-lab/
├── mcpp/            ⭐ 主项目:C++17 零依赖 MCP SDK(header-only,可编译可测试)
├── code-intel-mcp/  第二梯队:代码库索引/检索 MCP server(构建在 mcpp 之上)
├── docs/
│   ├── ROADMAP.md            10 周路线图 + 止损信号
│   └── designs/              副项目设计文档(v1 骨架,按窗口期启动)
└── .github/workflows/ci.yml  gcc/clang/msvc × linux/macos/windows
```

## 项目组合逻辑

| 项目 | 角色 | 状态 |
|---|---|---|
| **mcpp** | 地基:官方缺位的 C++ MCP SDK | v0.1 代码完成(JSON/协议/stdio transport/server/client/测试/示例/CI) |
| **code-intel-mcp** | 第一个杀手级应用:给 Claude Code/Cline 用的高速代码索引 | v0.1 骨架完成(词法扫描版),tree-sitter 升级路径已规划 |
| meeting-notes | 商业备选:离线会议纪要全 C++ pipeline | 设计文档 |
| mcp-ros2-bridge | 具身智能窗口期:MCP ↔ ROS2 | 设计文档 |
| edge-agent-runtime | 单二进制 agent runtime("agent 版 llama.cpp") | 设计文档 |

**自举闭环**:mcp-test(TS,已有)测 mcpp;cronagent(已有)调度 code-intel-mcp;agent-pr-gate(已有)守 CI——现有 TS 工具链直接服务新的 C++ 底座。

## 快速开始

```bash
# mcpp:构建 + 测试
cmake -S mcpp -B mcpp/build && cmake --build mcpp/build -j
ctest --test-dir mcpp/build --output-on-failure

# 用 MCP Inspector 试 echo server
npx @modelcontextprotocol/inspector ./mcpp/build/echo_server

# code-intel-mcp
cmake -S code-intel-mcp -B code-intel-mcp/build && cmake --build code-intel-mcp/build -j
```

## 详细背景

调研报告(生态地图、落地场景评估、为什么是这个方向):见
`../cpp-ai-agent-research.md`(工作区内)。战略结论:押注 C++ Agent 基础设施,
下行有保底(求职硬通货 + 自用地基),上行成为生态事实标准;止损信号是
官方发布 C++ SDK 或引擎厂商开源标配实现。

License:MIT(各子项目内)。
