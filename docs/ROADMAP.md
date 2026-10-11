# ROADMAP — C++ Agent 基础设施三部曲

> 原则:mcpp 是地基,code-intel-mcp 是证明(自用杀手级应用),每个副项目
> 只有在窗口期信号确认后才启动。所有代码零依赖优先,先可编译可测试再谈特性。

## 阶段 0:本周(已完成 ✅)

- [x] mcpp v0.1 代码:JSON 库 / 协议层 / stdio+内存 transport / server / client
- [x] 单元测试(JSON ~40 case)+ 协议集成测试(握手/工具/资源/提示词/错误路径/端到端)
- [x] 示例:echo、沙箱 filesystem、sysinfo、mini_client(可拉起任意 MCP server 验证)
- [x] code-intel-mcp v0.1:词法扫描索引 + 符号搜索 + grep + 分页读文件
- [x] CI 矩阵:linux(gcc/clang)/ macos / windows(msvc)

## 阶段 1:第 1-2 周 — 协议打穿与验收(部分完成 ✅)

- [x] Windows 本地 MinGW GCC 16.2 + CMake 4.4 全量构建,ctest 4/4 全绿
      (2026-10-11)
- [x] ProcessTransport 从示例转正进库(Win CreateProcess / POSIX fork-exec),
      含子进程集成测试
- [x] 官方 MCP Inspector CLI 实连验收:initialize / tools/list / tools/call
      全部通过(echo_server)
- [x] Streamable HTTP transport 首版(server 侧 POST-only、无状态、keep-alive);
      curl 全场景验证:200 JSON / 202 通知 / 405 非 POST / -32002 门禁
- [x] 基准测试落地:全链路 round-trip ~228k ops/s(单线程,详见 mcpp README)
- [ ] 用 MCP Inspector 验收 code-intel-mcp + HTTP transport(需 SSE GET 后)
- [ ] 用作者自己的 mcp-test(TS)对 mcpp server 做功能 + 安全测试(注入/越权路径)
- [ ] 建公开 GitHub repo,迁出 monorepo 首版(mcpp 单独成 repo 更利于传播)
- [ ] mac/linux CI 实跑(GitHub Actions 推送后自然验证)

## 阶段 2:第 3-6 周 — mcpp v0.2 + code-intel v0.2

**mcpp:**
- [ ] ProcessTransport 从示例转正(Windows/POSIX 子进程 + 双管道)
- [ ] Streamable HTTP transport(单 POST 端点,先 server 侧)
- [ ] 分页(tools/list cursor)、进度通知、取消钩子
- [ ] 基准测试:对比 TypeScript SDK 的 tools/call 吞吐与 P99(C++ 应显著赢)
- [ ] vcpkg / CMake FetchContent 发布

**code-intel-mcp:**
- [ ] tree-sitter 集成(C++/Python/TS 三语言精确解析,替换词法启发式)
- [ ] 增量重索引(mtime + 文件哈希)
- [ ] 符号引用图(find references / call graph v1)
- [ ] BM25 文本检索与符号搜索混合(嵌入模型路线放 v0.4)
- [ ] 基准:索引 mcpp+code-intel 自身源码 <1s;对标 Serena 冷启动

## 阶段 3:第 7-10 周 — 差异化与发布

- [ ] mcpp README 对比表(cpp-mcp / TinyMCP / mcp-cpp / TSAR-MCP)
- [ ] 技术博客:《为什么 MCP 需要 C++ SDK》+ 基准数据
- [ ] MCP 官方 Discord / GitHub discussions 提交,寻求 ecosystem 收录
- [ ] 发布 mcpp v0.2 + code-intel-mcp v0.2,目标 4 周 200+ 星
- [ ] Claude Code / Cline 实配 code-intel-mcp 的配置文档

## 副项目启动条件(不满足不开工)

| 项目 | 启动条件 | 预估工作量 |
|---|---|---|
| meeting-notes | 决定做商业路线时立即启动(whisper.cpp+llama.cpp+GUI 壳) | 8-12 周 MVP |
| mcp-ros2-bridge | mcpp v0.2 HTTP transport 就绪 + 装好 ROS2/Gazebo 环境 | 4-6 周 v0.1 |
| edge-agent-runtime | code-intel 发布后视社区反馈启动 | 10+ 周 |
| unreal-mcp 插件 | 有 Unreal 环境且确认兴趣 | 3-4 周 v0.1 |

## 止损信号(出现任一即重估整个方向)

1. Anthropic / 微软发布官方 C++ SDK → mcpp 转向差异化定位(零依赖嵌入式)或并轨
2. 主流引擎/机器人厂商把私有 MCP 实现开源为标配 → 生态位消失,转向 code-intel 纯应用层
3. MCP 被竞争协议取代(低概率,2026-10 观察:OpenAI/Google 均已兼容 MCP)
