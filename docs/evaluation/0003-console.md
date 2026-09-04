# 控制台（Console）方案评估

> **文档类型**：技术评估报告（非决策 ADR）
> **决策状态**：⏳ 待用户拍板（优先级低，Phase 3）
> **维护者**：Pride Leong
> **日期**：2026-08-10
> **关联**：[adr/0005](../adr/0005-lightweight-multiplatform.md) · [adr/0007](../adr/0007-api-spec-multi-language.md) · [adr/0010](../adr/0010-transport-shm-infra.md)

---

## 1. 用户需求

类似游戏中的控制台（Quake / Source engine console），打开后能：

1. **连接到任意运行中的 加载进程 节点**
2. **查看节点对象状态**（reader/writer/state/lineage）
3. **修改运行时参数**（SLA / queue_size / priority）
4. **触发动作**（手动 replay / 注入测试消息 / dump lineage）
5. **观测实时数据**（消息预览、延迟分布、SLA 健康度）
6. **多加载进程同时连接**（一台车有多个 加载进程）

控制台本身**是独立进程**，与 加载进程 解耦。

## 2. 典型使用场景

| 场景 | 频率 | 价值 |
|---|---|---|
| 开发调试：查某 channel 实时消息 | 高 | 替代 `cyber_monitor` |
| 现场调试：车端故障排查 | 中 | 替代远程 SSH + grep log |
| 性能调优：实时看 SLA 健康度 | 中 | 替代 `cyber_channel` + 自定义脚本 |
| 单元测试：注入测试消息验证算子 | 中 | 替代 `.record` 回放 |
| 演示/培训：图形化展示数据流 | 低 | 现场展示 |

## 3. 候选方案

### 方案 A：纯 RPC 风格

控制台通过 RPC（自研 / gRPC / Cap'n Proto RPC）连接 加载进程，每节点暴露 RPC 接口。

```
Console Process                  Mainboard Process
   │                                    │
   ├── list_channels ──RPC──────────→  │
   │                                    ├── return channels
   │←─────────────────response─────────┤
   │                                    │
   ├── inspect "/perception/front" ──→ │
   │                                    ├── return msg preview (serialize)
   │←─────────────────response─────────┤
```

**优点**：实现简单；跨机友好；标准模式。
**缺点**：每次请求都序列化；大数据预览（点云/图像）开销大；实时性受限。

### 方案 B：纯 SHM mirror

控制台启动时加入 加载进程的 SHM 区域，直接读 加载进程的 lineage / state buffer。

**优点**：零拷贝，高性能；大数据预览无开销。
**缺点**：仅同机；多加载进程 需要 SHM 联邦；写入难（SHM mirror 通常是单向）。

### 方案 C：嵌入式调试器风格（gdb attach / ptrace）

控制台 attach 到 加载进程，用 ptrace 读对象。

**优点**：完全通用（任何对象都可读）。
**缺点**：严重侵入（加载进程暂停）；性能影响大；不可在生产用。

### 方案 D：观测性 API（OpenTelemetry / Prometheus 风格）⭐

每节点暴露标准 metrics + commands，控制台订阅。

**优点**：标准化；生态丰富（Grafana 等）；可远程。
**缺点**：要预定义 metrics；对象级访问弱（只有指标，不直接看对象）。

### 方案 E：混合（RPC + SHM mirror + Observability）⭐ **推荐**

| 操作 | 实现 |
|---|---|
| 控制命令（list / set param / inject） | RPC（自研轻量协议，基于 Protobuf over unix socket） |
| 大数据预览（消息内容） | SHM mirror（控制台订阅 channel 的 SHM 旁路） |
| 实时指标（延迟 / SLA） | Observability（加载进程发布 metrics） |
| 对象深查（reader 内部状态） | RPC + debug API（加载进程主动暴露） |

## 4. 推荐方案详解（方案 E）

### 4.1 总体架构

```
┌────────────────────────────────────────────────────────┐
│              Console Process（独立进程）                  │
│                                                          │
│  ┌────────────┐  ┌────────────┐  ┌────────────┐         │
│  │ TUI/Web UI │  │ Cmd Parser │  │ Data View  │         │
│  └────────────┘  └────────────┘  └────────────┘         │
│         │                │              │                │
│         └────────────────┴──────────────┘                │
│                          │                               │
│              ┌──────────────────────┐                    │
│              │ Console Client SDK   │                    │
│              └──────────────────────┘                    │
└──────────────────────────┼─────────────────────────────┘
                            │
                       Unix Socket / TCP
                            │
┌──────────────────────────┼─────────────────────────────┐
│              Mainboard Process                          │
│                          │                               │
│              ┌──────────────────────┐                    │
│              │ Console Server       │                    │
│              │ (RPC + Pub/Sub)      │                    │
│              └──────────────────────┘                    │
│                          │                               │
│       ┌──────────┬───────┴────────┬───────────┐          │
│       │          │                │           │          │
│   ┌───▼──┐  ┌────▼───┐  ┌─────────▼────┐  ┌──▼───┐     │
│   │Node A│  │Node B  │  │ LineageQuery │  │ Stats│     │
│   └──────┘  └────────┘  └──────────────┘  └──────┘     │
│                                                          │
└──────────────────────────────────────────────────────────┘
```

### 4.2 控制协议（自研轻量 RPC）

基于 Protobuf + Unix Socket（同机）/ TCP（跨机）：

```protobuf
service ConsoleService {
  // 查询类
  rpc ListChannels(Empty) returns (ChannelList);
  rpc ListNodes(Empty) returns (NodeList);
  rpc InspectChannel(ChannelRequest) returns (ChannelState);
  rpc InspectNode(NodeRequest) returns (NodeState);
  rpc QueryLineage(LineageQuery) returns (LineageResult);
  rpc GetStats(StatsRequest) returns (StatsResponse);

  // 操作类
  rpc SetParam(SetParamRequest) returns (Status);
  rpc InjectMessage(InjectRequest) returns (Status);
  rpc TriggerReplay(ReplayRequest) returns (Status);
  rpc EnableBackend(BackendRequest) returns (Status);

  // 流式（pub/sub）
  rpc SubscribeChannel(ChannelRequest) returns (stream Message);
  rpc SubscribeStats(StatsRequest) returns (stream StatsResponse);
  rpc SubscribeLineage(LineageQuery) returns (stream LineageEvent);
}
```

### 4.3 大数据预览（SHM mirror）

控制台订阅某 channel 的 SHM 旁路：

```
Mainboard：消息发布时，除了主 channel，还写一份到 "/__console/mirror/<channel>" SHM 旁路（按需开启）
Console：从 SHM 旁路读消息，零拷贝预览
```

**按需开启**：只有控制台连上时才开 SHM 旁路，平时零开销。

### 4.4 安全模型

| 操作 | 权限 |
|---|---|
| List / Inspect / Subscribe | 只读，所有连接允许 |
| SetParam / Inject / Replay | 写入，需认证（unix socket peer cred 或 token） |
| EnableBackend / ShutdownNode | 危险，需 root 或特殊 token |

### 4.5 多语言支持（与 ADR-0007 协同）

控制台 SDK 走 C ABI，多语言客户端：

| 语言 | 实现 |
|---|---|
| C++ | 原生 |
| Python | pybind11 |
| Rust | cxx |
| Go | cgo |
| Node.js | napi-rs |

**TUI 推荐**：C++ 写（与天枢同语言），用 `ftxui` / `notcurses`。
**Web UI 推荐**：后端 C++ 暴露 HTTP/WebSocket，前端用 React + xterm.js。

## 5. 工作量估算

| ID | 描述 | 优先级 | 估算 | Phase |
|---|---|---|---|---|
| L4-CONSOLE-1 | ConsoleService Protobuf 定义 + 协议文档 | P2 | 2 | 3 |
| L4-CONSOLE-2 | 加载进程端 ConsoleServer（RPC + Pub/Sub） | P2 | 6 | 3 |
| L4-CONSOLE-3 | 加载进程端 Inspect/Query API（暴露 Node/Reader/Writer 状态） | P2 | 5 | 3 |
| L4-CONSOLE-4 | SHM mirror 旁路（按需开启） | P2 | 4 | 3 |
| L4-CONSOLE-5 | Console Client SDK（C ABI + C++） | P2 | 4 | 3 |
| L4-CONSOLE-6 | TUI（ftxui，命令行交互） | P2 | 6 | 3 |
| L4-CONSOLE-7 | Web UI（HTTP/WS + React，可选） | P3 | 10 | 3 |
| L4-CONSOLE-8 | 安全模型（peer cred / token） | P2 | 2 | 3 |
| L4-CONSOLE-9 | Python/Rust SDK 适配（基于 C ABI） | P3 | 4 | 3 |
| L4-CONSOLE-10 | 集成测试 + 示例场景 | P2 | 3 | 3 |
| **合计** | - | - | **46 点** | **Phase 3** |

## 6. 与现有架构的协同

| ADR | 协同点 |
|---|---|
| [adr/0007 多语言 SDK](../adr/0007-api-spec-multi-language.md) | Console Client SDK 走相同 C ABI |
| [adr/0008 消息多格式](../adr/0008-message-format-multi.md) | Inspect 返回的 message preview 支持三格式 |
| [adr/0010 通用基础设施](../adr/0010-transport-shm-infra.md) | SHM mirror 用 ShmPool；RPC 走 INTRA（同机）/ Zenoh（跨机） |
| [adr/0005 profile](../adr/0005-lightweight-multiplatform.md) | 控制台仅在 desktop/server/vehicle profile 启用，embedded/mcu 不支持 |

## 7. 推荐结论

**采用方案 E（混合：RPC + SHM mirror + Observability）**，工作量 46 点，全部 Phase 3（低优先级，不阻塞 Phase 1/2）。

### 关键设计决策

1. **控制协议走自研 RPC**（不用 gRPC，避免依赖）：基于 Protobuf + Unix Socket
2. **大数据预览走 SHM mirror**（按需开启）：零拷贝
3. **指标走 Observability**（标准化）：未来可与 Prometheus/Grafana 集成
4. **TUI 优先，Web UI 可选**：车端现场用 TUI，开发桌面可加 Web UI
5. **多加载进程同时连接**：每加载进程 独立 ConsoleService，控制台聚合视图

## 8. 待用户拍板的 fork

### Fork 1：方案选型

| 选项 | 描述 | 工作量 |
|---|---|---|
| **E（推荐）** | 混合（RPC + SHM mirror + Observability） | 46 点 |
| A | 纯 RPC | 25 点 |
| B | 纯 SHM mirror | 20 点（仅同机） |
| D | 纯 Observability | 15 点（功能受限） |

### Fork 2（如果选 E）：UI 形态

- E1. TUI only（ftxui，命令行）→ 6 点
- **E2. TUI + Web UI**（车端 TUI + 桌面 Web）→ 16 点
- E3. Web only（浏览器）→ 10 点

### Fork 3：控制台运行环境

- C1. 仅 desktop/server（开发调试）
- **C2. 加 vehicle**（车端现场）→ 推荐但 TUI 受限
- C3. 全 profile（含 embedded/mcu）→ 不推荐

## 9. 决策后影响

如果用户选 E + E2 + C2：

| 影响项 | 变更 |
|---|---|
| 02-development-plan | 新增 L4-CONSOLE-1..10（46 点，全部 Phase 3） |
| ADR | 升级本文档为 ADR-0011 |
| L4-TRANS | ConsoleService 复用 TransportBackend（详见 [adr/0010](../adr/0010-transport-shm-infra.md)） |
| L2-LIN | LineageQuery API 新增（控制台查询用） |
| L3-DETECT | Stats 发布接口（控制台订阅用） |

## 10. 附录：参考实现

### 游戏控制台

- **Quake / Source engine console**：命令行 + cvar 系统
- **Unreal Engine Console**：Python + Blueprint 联动
- **Cyberpunk REDkit Console**：实时对象查看

### 机器人中间件

- **ROS 2 rqt**：Qt-based GUI 工具，主题/服务调用
- **CyberRT cyber_monitor / cyber_channel**：CLI 工具
- **Apex.OS Developer Tools**：商业化 GUI

### 工业控制台

- **OpenTelemetry Collector**：观测性标准
- **Prometheus + Grafana**：指标可视化
- **Jaeger / Tempo**：分布式追踪

## 11. 参考资料

- Protobuf: https://protobuf.dev/
- ftxui: https://arthursonzogni.github.io/ftxui/
- notcurses: https://notcurses.com/
- TUI 借鉴：https://github.com/ratatui-org/ratatui（Rust 实现，可参考设计）
- xterm.js: https://xtermjs.org/
