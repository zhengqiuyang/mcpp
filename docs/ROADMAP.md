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
- [x] 用作者自己的 mcp-test(TS)对 mcpp server 做功能 + 安全测试:6/6 功能 +
      16/16 安全探测 0 泄漏(2026-10-11,配置固化在 mcpp/tests/mcp-test.yaml,进 CI)
- [x] HTTP 客户端传输 HttpClientTransport(修复两处真实 bug:TCP 分段导致的
      堆越界读、通知消息丢失),端到端测试 test_http 9/9
- [x] mcpp vs 官方 TS SDK 对比基准:全链路 228k vs 24.8k ops/s(9.2×),
      脚本固化在 benchmarks/ts-sdk/
- [ ] 建公开 GitHub repo,迁出 monorepo 首版(mcpp 单独成 repo 更利于传播)
- [ ] mac/linux CI 实跑(GitHub Actions 推送后自然验证)

## 阶段 2:第 3-6 周 — mcpp v0.2 + code-intel v0.2(2026-10-12 完成 ✅)

**mcpp(全部完成):**
- [x] ProcessTransport 转正(Win/POSIX 子进程 + 双管道)+ 子进程集成测试
- [x] Streamable HTTP:server POST-only + keep-alive + Mcp-Session-Id 会话
      (mint/校验/DELETE),client 自动附带会话头 + end_session()
- [x] 分页(cursor/nextCursor)、进度通知(_meta.progressToken)、取消检查点(-32800)
- [x] CMake install(EXPORT)/find_package 消费者端到端验证 + FetchContent 文档
- [x] 基准对比 TS SDK:9.2×(228k vs 24.8k ops/s)

**code-intel-mcp(全部完成):**
- [x] tree-sitter vendored(C/C++/Python/TS/TSX 五语法,离线构建)
- [x] 精确符号提取(构造/析构/operator=),28 项单测
- [x] 增量重索引(mtime,单测覆盖)

## 加固冲刺(v0.2.1,2026-10-12 ✅)

- [x] 服务端参数校验(默认开启,-32602);基准更新为"双侧都带校验"的对等对比:11×
- [x] 确定性 fuzz(fuzz_json 30 万次零失败 + fuzz_rpc 5 万次)进 ctest 与 CI;
      **fuzz 首跑即抓到真实 bug**:数字溢出到 infinity 时往返断裂(已修+回归用例)
- [x] HTTP 会话加固:TTL 过期、并发上限(503)、Origin 守卫(403,DNS-rebinding 防护)
- [x] **加固测试抓到 v0.1 遗留 bug**:Connection: close 头解析 off-by-one 导致
      keep-alive 误判(已修,`header_says_keep_alive` 改用 needle.size())
- [x] CI 新增 sanitize job(ASan+UBSan 全量测试 + 加长 fuzz)
- [x] code-intel:纯符号名(operator=/析构无签名噪声)、文件内容缓存、
      references 工具(全词匹配,def/use 标注)、预计算小写搜索

## 阶段 3:第 7-10 周 — 差异化与发布(部分进行中)

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
