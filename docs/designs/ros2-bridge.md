# design: mcp-ros2-bridge(具身智能窗口期)

让任何 MCP client(Claude 等)直接监控/操控 ROS2 节点——topics/services/actions
映射为 MCP tools/resources,内置安全白名单。对标论文阶段的 RoboNeuron,
差异点:零 Python 桥、控制回路内原生 C++、安全模型开箱即用。

## 动机

- 2026 具身智能是最热赛道,但 agent ↔ 机器人中间件缺人做
- 现有做法:ROS2 节点旁边挂 Python MCP 桥(rclpy),延迟高、部署重
- rclcpp 是 C++:桥接层可以和 ROS2 节点同进程,零跨语言开销
- 开发无需自持硬件:Gazebo/Isaac Sim 仿真即可

## 架构

```
Claude / 任意 MCP client
        │ MCP (stdio 或 Streamable HTTP, mcpp v0.2)
        │
mcp-ros2-bridge(rclcpp node + mcpp server 同进程)
        │ 自动发现 /introspection
        ├─ topics → MCP resources(可订阅: /tf, /camera/image_raw …)
        ├─ services/actions → MCP tools(/navigate_to_pose …)
        └─ 参数/节点生命周期 → 管理工具(list_nodes, launch/kill)
安全层:白名单 URI 模式 + 工具调用审计日志 + 只读模式开关
```

## 工具面 v0.1

| 工具 | 说明 |
|---|---|
| `list_nodes` / `list_topics` / `list_services` | ROS2 图发现 |
| `read_topic {topic, n}` | 采样 n 条消息(JSON 化,图片返回 base64 缩略) |
| `call_service {service, args}` | 白名单内的服务调用 |
| `pub_topic {topic, type, msg}` | 仅白名单 + 显式 `--allow-write` 时启用 |
| `nav_to {x,y,yaw}` | 便捷封装(调 /navigate_to_pose action)|

## 安全模型(差异化的关键)

- 默认只读;任何写操作要求启动参数显式授权
- 白名单用通配 URI 模式(`/cmd_vel` 默认禁止,`/nav/*` 按需放行)
- 全量审计 JSONL:谁(agent/client)、何时、调了什么、结果
- 复用作者 mcp-test 的安全测试思路:注入用例进 CI

## 里程碑

1. W1-2:bridge 骨架(rclcpp + mcpp stdio)+ list_* 三件套
2. W3-4:read_topic / call_service + 安全层 + 审计
3. W5-6:TurtleSim/Gazebo 演示("用 Claude 开遥控乌龟")+ 文档视频

## 前置条件(见 ROADMAP 启动门槛)

mcpp v0.2 HTTP transport 就绪;本机装 ROS2(Jazzy/rolling)+ Gazebo。
