# design: meeting-notes(商业备选路线)

完全离线的本地会议纪要——whisper.cpp 流式转录 + llama.cpp 摘要 + 可选 TTS 回放,
打包为单二进制桌面应用。隐私是核心卖点:零数据出设备。

## 为什么商业上成立

- 付费需求已被验证(Local Whisper、ScribeAI 等 macOS/iOS 付费 App)
- 现有竞品要么云依赖(隐私焦虑)、要么功能割裂(只转录不纪要)
- C++ 单二进制 = 无 Python 环境、无安装向导、低内存占用,分发成本最低

## 架构(目标形态)

```
[音频捕获] → [VAD 分段] → [whisper.cpp 流式 ASR] → [说话人分离]
                                                      ↓
[本地 agent(mcpp client)] ← [llama.cpp 摘要/行动项/邮件草稿]
        ↓ 可调用 MCP 工具:写文件、日历、邮件草稿(经用户确认)
[SQLite 存储] + [GUI: Tauri 壳(前端复用你 TS 技能)]
```

技术栈:whisper.cpp + llama.cpp(llama-server OpenAI 兼容 API)+ sqlite-vec
(历史会议语义检索)+ mcpp(工具调用层)+ Tauri GUI。

## MVP 范围(8-12 周)

1. 录音 → WAV → 转录 → 摘要/行动项(先离线文件模式,不做实时)
2. 单说话人,中文 + 英文
3. 导出 Markdown 会议纪要
4. macOS + Windows

砍掉:实时字幕、说话人分离、TTS(进 v2)、多语言 UI。

## 商业化路径

定价参考:一次性 $29-49 或订阅 $5/月;渠道 Gumroad + Setapp;
增长叙事:"你的会议永远不离开你的电脑"。

## 风险

- 竞品跟进:大厂(Granola 等)云方案降维打击 → 差异化坚持离线
- 模型质量:3B 级本地模型摘要质量不够 → 允许配置"外接 API 可选"
  (默认本地,用户自选)

## 与主线的协同

会议后 agent 自动跟进(生成邮件草稿、建任务)= mcpp 的真实用户场景,
反向驱动 SDK 打磨。
