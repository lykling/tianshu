# ADR-0014：控制台（Console）

- **状态**：**设计已接受**，实现 Phase 3
- **日期**：2026-08-10
- **决策者**：Pride Leong
- **关联**：[evaluation/0003](../evaluation/0003-console.md) · [adr/0007](./0007-api-spec-multi-language.md) · [adr/0010](./0010-transport-shm-infra.md) · [adr/0013](./0013-cross-machine-transport.md)

---

## 决策

按 [evaluation/0003](../evaluation/0003-console.md) 推荐方案 E 落地：

| Fork | 选定 |
|---|---|
| 方案 | **E**：混合（自研 RPC + SHM mirror + Observability） |
| UI 形态 | **E2**：TUI + Web |
| profile 范围 | **C3**：全平台（与框架本身 profile 一致） |
| 跨机访问 | **必须支持**（通过 TransportBackend 抽象，console client 可远程连接任意 mainboard） |
| 设计完成度 | **Phase 0-2 设计完整**，实现 Phase 3 |

## 核心定位

控制台是**管理调试入口**，建立在通信基础上：

- 独立进程，可连接任意运行中的 mainboard
- 跨机访问（多 mainboard 联合调试）
- 多语言 SDK（Python/Rust/Go/Node 都能用）
- 全 profile 支持（含 embedded / mcu 的精简变体）

## 总体架构

```
┌─────────────────────────────────────────────────────────┐
│ Console Process（独立进程）                               │
│                                                           │
│  ┌──────────┐  ┌──────────┐  ┌──────────────────────┐   │
│  │ TUI      │  │ Web UI   │  │ CLI（脚本/自动化）    │   │
│  │ (ftxui)  │  │ (React)  │  │                      │   │
│  └────┬─────┘  └────┬─────┘  └──────────┬───────────┘   │
│       └─────────────┴───────────────────┘               │
│                     ↓                                     │
│       ┌──────────────────────────────┐                   │
│       │ Console Client SDK（C ABI）  │                   │
│       │  - 多 mainboard 连接管理      │                   │
│       │  - 命令分发 / 响应聚合        │                   │
│       │  - 多语言绑定（pybind11/cxx）│                   │
│       └──────────────────────────────┘                   │
└─────────────────────┬───────────────────────────────────┘
                      │
            TransportBackend（详见 ADR-0010）
            ├── INTRA（同机同进程，仅控制台嵌入场景）
            ├── SHM（同机跨进程）
            └── Zenoh（跨机，详见 ADR-0013）
                      │
┌─────────────────────┴───────────────────────────────────┐
│ Mainboard Process                                        │
│   ┌──────────────────────────────────┐                  │
│   │ ConsoleServer                    │                  │
│   │  - 自研 RPC（Protobuf over sock）│                  │
│   │  - Pub/Sub（实时数据流）          │                  │
│   │  - 安全（peer cred / token）     │                  │
│   └──────────────────────────────────┘                  │
│   ┌──────────────────────────────────┐                  │
│   │ Inspect / Query / Stats API      │                  │
│   │  - 暴露 Node / Reader / Writer   │                  │
│   │  - LineageQuery（与 L2-LIN 协同）│                  │
│   │  - Stats 发布（与 L3-DETECT 协同）│                  │
│   └──────────────────────────────────┘                  │
└──────────────────────────────────────────────────────────┘
```

## 控制协议（自研 RPC，不用 gRPC）

避免 gRPC 重依赖（与 [ADR-0005 轻架构](./0005-lightweight-multiplatform.md) 协同）。基于 Protobuf + Unix Socket（同机）/ Zenoh（跨机）：

```protobuf
service ConsoleService {
  // 查询类（同步）
  rpc ListChannels(Empty) returns (ChannelList);
  rpc ListNodes(Empty) returns (NodeList);
  rpc ListMainboards(Empty) returns (MainboardList);
  rpc InspectChannel(ChannelRequest) returns (ChannelState);
  rpc InspectNode(NodeRequest) returns (NodeState);
  rpc InspectParam(ParamRequest) returns (ParamValue);
  rpc QueryLineage(LineageQuery) returns (LineageResult);
  rpc GetStats(StatsRequest) returns (StatsResponse);

  // 操作类（同步）
  rpc SetParam(SetParamRequest) returns (Status);
  rpc InjectMessage(InjectRequest) returns (Status);
  rpc TriggerReplay(ReplayRequest) returns (Status);
  rpc EnableBackend(BackendRequest) returns (Status);
  rpc ShutdownNode(ShutdownRequest) returns (Status);

  // 流式（Pub/Sub）
  rpc SubscribeChannel(ChannelRequest) returns (stream Message);
  rpc SubscribeStats(StatsRequest) returns (stream StatsResponse);
  rpc SubscribeLineage(LineageQuery) returns (stream LineageEvent);
}
```

**传输**：

| 场景 | 传输 |
|---|---|
| 同机（开发） | Unix Socket |
| 同机（生产） | SHM（高性能数据预览） |
| 跨机 | Zenoh（详见 ADR-0013） |

## 大数据预览（SHM mirror）

控制台订阅某 channel 时，mainboard 把消息额外写到 SHM 旁路（按需开启）：

- 控制台零拷贝读
- 平时零开销（无人订阅时不开 mirror）
- 支持节流（如最多 30 FPS 给控制台，避免影响主链路）

## 安全模型

| 操作 | 权限 |
|---|---|
| List / Inspect / Subscribe | 只读，所有连接允许 |
| SetParam / Inject / Replay | 写入，需认证（unix socket peer cred 或 token） |
| EnableBackend / ShutdownNode | 危险，需 root 或特殊 token |

## UI 形态

### TUI（ftxui，命令行）

- 类似 htop / lazygit 的命令行交互
- 默认视图：channel 列表 + 实时延迟 / SLA
- 命令模式：`:` 进入命令行（list / inspect / set / inject）
- 多 mainboard 切换：tab 键
- 适用：desktop / server / vehicle（车端现场）

### Web UI（React + xterm.js）

- 浏览器访问（mainboard 监听 HTTP/WS）
- 实时图表（Grafana-like）
- 终端模拟器（xterm.js，用于 REPL）
- 适用：desktop / server

### profile 启用

| Profile | TUI | Web UI | 备注 |
|---|---|---|---|
| desktop | ✅ | ✅ | 完整 |
| server | ✅ | ✅ | 完整 |
| vehicle | ✅ | ❌ | 仅 TUI（资源约束） |
| embedded | ⚠️（简化） | ❌ | 仅基础查询 |
| mcu | ❌ | ❌ | 不支持（资源装不下） |

**全平台支持**通过框架层抽象实现：MCU 不直接支持控制台，但 mcu 上的 mainboard 可以被**远程** desktop/server 上的控制台连接（通过 Zenoh-pico）。

## 多语言 SDK

通过 C ABI 暴露（与 [ADR-0007](./0007-api-spec-multi-language.md) 协同）：

```python
import tianshu.console as tc

# 连接多个 mainboard
session = tc.connect(["tcp://192.168.1.10:7447", "tcp://192.168.1.11:7447"])

# 列举所有 channel
for ch in session.list_channels():
    print(ch.name, ch.msg_type, ch.publishers)

# 实时订阅
sub = session.subscribe("/perception/front")
for msg in sub:
    print(msg.seq, msg.latency_ns)

# 修改参数
session.set_param("scheduler_cpu_count", 8)

# 触发 replay
session.trigger_replay("/perception/front", start_seq=1000, end_seq=2000)
```

## 工作量（46 点，全部 Phase 3）

详见 [02-development-plan F-L4-CONSOLE](../02-development-plan.md)。

## 影响范围

### 与其他模块协同

| 模块 | 协同 |
|---|---|
| L4-TRANS（ADR-0010） | 控制台走 TransportBackend 抽象 |
| L2-LIN-4 LineageQuery | 控制台 query 走此 API |
| L3-DETECT-2 SLA 监控 | 控制台订阅 stats |
| INFRA-PARAM-10 热加载 | 控制台 set_param 走此 API |
| INFRA-LOG（ADR-0011） | 控制台订阅日志 |

## 风险与缓解

| 风险 | 缓解 |
|---|---|
| 控制台影响主链路性能 | SHM mirror 按需开启 + 节流策略 |
| 跨机控制台延迟 | 接受 ms 级；高频数据走订阅 |
| 安全（远程操控车端） | 强制 token + TLS（Zenoh 原生支持） |
| 控制台进程崩溃 | 不影响 mainboard；重启即恢复 |
| 多 mainboard 状态聚合复杂 | Client SDK 做聚合，UI 看统一视图 |

## 后续可能演进

- 如果 Web UI 强需求 → 加 Grafana 集成（Prometheus exporter）
- 如果 OTel 强需求 → 适配 OpenTelemetry Logs/Traces/Metrics API
- 如果车端场景强需求 → 加 OTA 集成（远程升级 + 控制台确认）

## 附录：对象追踪设计（TraceRegistry）

### 原则：外部注册表，不做 Traceable 基类

控制台需要"列出所有活跃对象 + 检查状态"。不通过继承 `Traceable` 基类实现，
而是用**外部 TraceRegistry**（注册表模式）。

### 方案选型

| 方案 | 开销 | POD 兼容 | SHM 兼容 | 评价 |
|---|---|---|---|---|
| ❌ Traceable 基类（vtable） | 每实例 +8B vtable | ❌ 破坏 POD | ❌ vtable 跨进程不安全 | 反模式 |
| ✅ 外部 TraceRegistry | 追踪关闭时零开销 | ✅ | ✅ | 与 lineage 设计一致 |

### 设计

```cpp
namespace tianshu::trace {

struct ObjectMeta {
  uint64_t id;
  std::string_view type_name;
  void* ptr;
  std::chrono::steady_clock::time_point created;
};

class TraceRegistry {
 public:
  static TraceRegistry& instance();

  // 追踪开关（默认 OFF，零开销）
  bool is_enabled() const;
  void set_enabled(bool on);

  // 注册/注销（只在 enabled 时有开销）
  uint64_t register_object(void* ptr, std::string_view type);
  void unregister_object(uint64_t id);

  // 查询（控制台用）
  std::vector<ObjectMeta> list_all() const;
  std::vector<ObjectMeta> list_by_type(std::string_view type) const;
};

// RAII 自动注册/注销
class ScopedTrace {
 public:
  ScopedTrace(void* ptr, std::string_view type);
  ~ScopedTrace();
};

}  // namespace tianshu::trace
```

### 用法

```cpp
template <MessageConcept TMsg>
class Reader {
 public:
  Reader(...) {
    if (trace::TraceRegistry::instance().is_enabled()) {
      trace_ = std::make_unique<trace::ScopedTrace>(this, "Reader<...>");
    }
  }
 private:
  std::unique_ptr<trace::ScopedTrace> trace_;  // disabled 时 nullptr
};
```

### 为什么不做 Traceable 基类

1. **零开销原则**：vtable 每实例 +8B，对高频对象（每帧消息）不可接受
2. **POD 兼容**：ADR-0008 POD 消息不能用虚函数表
3. **SHM 兼容**：vtable 指针跨进程不安全（指向不同进程的 .rodata）
4. **编译期开关**：TraceRegistry 可以完全编译掉（`#ifdef TIANSHU_ENABLE_TRACING`），
   Traceable 基类无法编译掉
5. **与 lineage 一致**：L2-LIN 血缘也是外部追踪，不是侵入式

### 与 lineage 的关系

| 系统 | 追踪什么 | 粒度 |
|---|---|---|
| TraceRegistry | 对象实例（哪个 Reader 存在、谁创建的） | 对象级 |
| L2-LIN 血缘 | 消息流（每条消息从哪来、到哪去） | 消息级 |

控制台同时查询两者。

### 实现时序

| Phase | 做什么 |
|---|---|
| Phase 0/1 | **不实现**。ObjectPool/CacheBuffer/Node 正常写，不加追踪代码 |
| Phase 2 | 实现 TraceRegistry + 在 Node/Reader/Writer 加 `ScopedTrace` |
| Phase 3 | 控制台通过 TraceRegistry 查询对象 |
