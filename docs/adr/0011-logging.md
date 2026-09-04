# ADR-0011：日志规范

- **状态**：已接受
- **日期**：2026-08-10
- **决策者**：Pride Leong
- **关联**：[adr/0005](./0005-lightweight-multiplatform.md) · [adr/0007](./0007-api-spec-multi-language.md) · [adr/0009](./0009-doc-code-language.md) · [adr/0010](./0010-transport-shm-infra.md)

---

## 背景

天枢需要规范日志系统。问题：

| 痛点 | 现状风险 |
|---|---|
| 日志 API 不统一 | 不同模块各用 std::cout / printf / 自研 logger，难以全局调级 |
| 性能开销不可控 | 同步 I/O 阻塞 hot path，影响 H2 假设（< 1% 差距） |
| 难以多语言共享 | Python/Rust/Go/Node SDK 各自打日志，跨语言 trace 断裂 |
| 难以过滤聚合 | 没有结构化字段，grep 难；接入 ELK/Loki 需要解析 |
| profile 不友好 | vehicle 需要 syslog，mcu 需要串口，desktop 需要彩色 stderr |
| 多加载进程难追溯 | 不知道哪条日志来自哪个加载进程 / node |
| 与血缘脱节 | 日志和消息流无法对齐（lineage 已有 ts/src，日志没集成） |

[adr-0005 依赖治理](./0005-lightweight-multiplatform.md) 已把 glog 列入禁用清单（重 + 平台耦合）。本 ADR 设计天枢自研日志系统。

## 候选方案

### 方案 1：直接用 spdlog

industry 标准 header-only C++ logger。

**优点**：成熟；性能好；格式灵活。
**缺点**：依赖（虽然轻量）；fmt 依赖；不能跨多语言 SDK 统一；中文/英文混排格式不一致风险。

### 方案 2：stdio + 自定义格式（cyber 风格）

`AERROR << "msg" << var;` 这种 stream 风格。

**优点**：零依赖；与 cyber 相似，团队熟悉。
**缺点**：性能差（同步 I/O）；不能结构化；与现代日志聚合生态脱节。

### 方案 3：自研结构化 logger（**已选**）

借鉴 spdlog / folly Logger / OpenTelemetry Logs API 设计，自研天枢 logger，满足：

- 零第三方依赖（与 ADR-0005 协同）
- 结构化（key-value，JSON 友好）
- 异步默认（不阻塞 hot path）
- 多 sink 可插拔
- C ABI 暴露（与 ADR-0007 多语言 SDK 协同）
- profile 感知
- 与血缘集成

## 决策

**选方案 3**：自研结构化异步 logger。

## 核心 API 设计

### 日志级别

```cpp
enum class LogLevel : uint8_t {
  TRACE = 0,    // 极细粒度（每条消息都打）
  DEBUG = 1,    // 调试
  INFO  = 2,    // 关键里程碑（默认级别）
  WARN  = 3,    // 异常但可恢复
  ERROR = 4,    // 错误（用户可见）
  FATAL = 5,    // 致命（终止进程）
};
```

### 宏 API（推荐用法）

```cpp
TIANSHU_TRACE("channel_ready") << "channel " << channel << " ready";
TIANSHU_DEBUG("scheduler") << "task " << id << " queued";
TIANSHU_INFO("launch") << "started, flows=" << flow_names.size();
TIANSHU_WARN("sla") << "deadline miss count=" << miss_count;
TIANSHU_ERROR("transport") << "shm attach failed: " << path;
TIANSHU_FATAL("init") << "config parse failed, aborting";
```

第一个标识符是 **scope**（模块/子系统名），用于过滤。日志输出格式：

```
2026-08-10T15:30:45.123456+08  INFO  scheduler    task=42 queued
```

### 结构化字段 API（机器友好）

```cpp
TIANSHU_INFO("msg_received")
    .field("channel", channel_name)
    .field("seq", seq)
    .field("size_bytes", payload_size)
    .field("latency_ns", latency_ns)
    .log("message received");
```

输出 JSON 行：

```json
{"ts":"2026-08-10T15:30:45.123456+08","level":"INFO","scope":"msg_received","msg":"message received","channel":"/perception/front","seq":42,"size_bytes":1024,"latency_ns":1234}
```

### 条件日志（性能优化）

```cpp
// 编译期剔除（PROFILE 关闭时不进二进制）
TIANSHU_TRACE_IF("hot_path", enable_trace, ...) << "...";

// 采样日志（避免高频淹没）
TIANSHU_INFO_EVERY_N("scheduler", 1000) << "heartbeat " << n;
TIANSHU_INFO_FIRST_N("init", 3) << "first 3 inits";
```

## 架构

```
┌─────────────────────────────────────────────────┐
│ 用户代码                                          │
│   TIANSHU_INFO("scope").field(...).log("msg")   │
└────────────────────┬────────────────────────────┘
                     ↓
┌─────────────────────────────────────────────────┐
│ Logger API（header-only，零开销编译期过滤）        │
│   - 宏展开为 LogRecord                            │
│   - 级别 / scope 编译期过滤                        │
└────────────────────┬────────────────────────────┘
                     ↓
┌─────────────────────────────────────────────────┐
│ LogRouter（单例，路由到 sink）                     │
│   - 同步 / 异步切换                                │
│   - scope → sink 映射                             │
│   - 速率限制（避免日志风暴）                       │
└────────────────────┬────────────────────────────┘
                     ↓
┌──────────┬──────────┬──────────┬──────────┬─────┴──────┐
│ Console  │ File     │ Syslog   │ Network  │ SHM Ring   │
│ Sink     │ Sink     │ Sink     │ Sink     │ Buffer     │
│ (stderr) │ (.log)   │ (systemd)|(UDP)     │ (lineage)  │
└──────────┴──────────┴──────────┴──────────┴────────────┘
```

### Sink 列表

| Sink | 用途 | profile |
|---|---|---|
| `ConsoleSink` | 彩色 stderr（开发） | desktop / server |
| `FileSink` | 滚动文件（按 size/time） | 全 profile |
| `SyslogSink` | systemd journal | vehicle / server |
| `NetworkSink` | UDP/TCP 推送到远端 | server（可选） |
| `ShmRingBufferSink` | SHM 环形缓冲（与 lineage 集成） | 全 profile |
| `SerialSink` | MCU 串口输出 | mcu |
| `NullSink` | 静默 | 测试用 |

### 异步模型

- 默认**异步**：用户线程写入无锁 MPSC 队列，后台 logger 线程消费
- 队列满时降级（按级别丢弃：先丢 TRACE/DEBUG，最后丢 INFO）
- Hot path 调用开销：< 200ns（无 I/O，仅内存写入）
- 关键路径可选**同步**：FATAL 必须同步（保证 flush 后退出）

### 多语言 SDK（与 ADR-0007 协同）

C ABI：

```c
// ffi/log_c.h
typedef struct tianshu_logger_t* tianshu_logger_handle;

tianshu_logger_handle tianshu_log_get(const char* scope);
void tianshu_log_log(tianshu_logger_handle logger, int level, const char* msg);
void tianshu_log_field(tianshu_logger_handle logger, const char* key, const char* val);
```

Python SDK：

```python
import tianshu
log = tianshu.log.get("perception")
log.info("object detected", channel="/front", count=5)
```

跨语言日志最终走同一个 LogRouter，统一 sink、统一格式。

### profile 默认配置

| Profile | 默认级别 | 默认 sink | 异步队列 |
|---|---|---|---|
| desktop | DEBUG | Console + File | 4MB |
| server | INFO | File + Network（可选） | 16MB |
| vehicle | INFO | File + Syslog | 4MB |
| embedded | WARN | File | 1MB |
| mcu | ERROR | Serial | 32KB |

## 与现有 ADR 协同

| ADR | 协同点 |
|---|---|
| [adr-0005 轻架构](./0005-lightweight-multiplatform.md) | 自研 logger 替代 glog；零第三方依赖 |
| [adr-0007 多语言 SDK](./0007-api-spec-multi-language.md) | C ABI 暴露 logger，多语言统一 |
| [adr-0009 双语+英文 commit](./0009-doc-code-language.md) | 日志消息必须英文（同代码注释） |
| [adr-0010 通用基础设施](./0010-transport-shm-infra.md) | ShmRingBufferSink 用 ShmPool 分配；lineage 集成 |

## 日志规范

### 内容规范

| 项 | 规范 |
|---|---|
| 语言 | **英文**（与 ADR-0009 一致） |
| scope 命名 | lowercase_with_underscores，模块/子系统名（`scheduler` / `transport_shm` / `dsl`） |
| msg 内容 | 简洁陈述句，不含变量值（变量走 field） |
| msg 大小 | < 80 字符（field 不限） |
| 时区 | ISO 8601 with timezone（`2026-08-10T15:30:45.123456+08`） |
| 字段命名 | lowercase_with_underscores（`channel_name` / `seq` / `latency_ns`） |
| 字段值类型 | primitive + string；复杂对象走序列化（不算 field） |

### 反模式

❌ 错误：

```cpp
// 含变量值在 msg 中（不利于聚合）
TIANSHU_INFO("scheduler") << "task 42 queued at 15:30";

// 中文日志（违反 ADR-0009）
TIANSHU_ERROR("init") << "初始化失败";

// 无 scope（无法过滤）
TIANSHU_INFO << "hello";

// hot path 同步打大量日志
for (auto& msg : hot_messages) {
  TIANSHU_INFO("hot") << msg.seq;  // 性能杀手
}
```

✅ 正确：

```cpp
TIANSHU_INFO("scheduler").field("task_id", 42).log("task queued");

TIANSHU_ERROR("init").field("reason", err_msg).log("init failed");

TIANSHU_INFO("unknown_scope") << "...";  // 禁用 - 必须有 scope

TIANSHU_INFO_EVERY_N("hot", 1000).field("batch", n).log("processing");
```

### 级别使用约定

| 级别 | 用途 | 示例 |
|---|---|---|
| TRACE | 极细粒度（仅 TRACE 模式启用） | 单条消息字段值 |
| DEBUG | 调试用（默认 desktop 启用） | 状态机转移、调度决策 |
| INFO | 关键里程碑（默认级别） | ti launch 启动、flow 注册、节点上线 |
| WARN | 异常但可恢复 | deadline 接近、buffer 接近满、retry |
| ERROR | 错误（用户可见） | 配置错误、消息丢失、连接断开 |
| FATAL | 致命 | init 失败、不可恢复状态（同步 flush 后 abort） |

## 影响范围

### 新增 INFRA 框架

| 框架 | 作用 |
|---|---|
| F-INFRA-LOG | Logger API + LogRouter + Sinks + 多语言 SDK |

### 工作量估算

| ID | 描述 | 优先级 | 估算 | Phase |
|---|---|---|---|---|
| INFRA-LOG-1 | Logger API（宏 + LogRecord + 编译期过滤） | P0 | 3 | 0-1 |
| INFRA-LOG-2 | LogRouter（单例 + 异步队列 + 速率限制） | P0 | 4 | 1 |
| INFRA-LOG-3 | ConsoleSink（彩色 stderr） | P0 | 1 | 1 |
| INFRA-LOG-4 | FileSink（滚动文件，按 size/time） | P0 | 2 | 1 |
| INFRA-LOG-5 | ShmRingBufferSink（与 lineage 集成） | P1 | 3 | 2 |
| INFRA-LOG-6 | SyslogSink（systemd journal） | P1 | 2 | 2 |
| INFRA-LOG-7 | NetworkSink（UDP/TCP 远端推送） | P2 | 2 | 3 |
| INFRA-LOG-8 | SerialSink（MCU 串口） | P2 | 2 | 3 |
| INFRA-LOG-9 | C ABI 暴露 + 多语言 SDK 适配 | P1 | 3 | 2 |
| INFRA-LOG-10 | profile 默认配置 + 文档 | P0 | 1 | 1 |
| INFRA-LOG-11 | 日志聚合集成示例（Loki/Promtail） | P2 | 2 | 3 |
| **合计** | - | - | **25 点** | - |

## 风险与缓解

| 风险 | 缓解 |
|---|---|
| 自研 bug 导致日志丢失 | 单测覆盖 + 与 spdlog benchmark 对比 |
| 异步队列满导致日志丢 | 按 level 优先级丢（先 TRACE/DEBUG，最后 INFO） |
| 多语言 SDK 日志格式不一致 | C ABI 强制走 LogRouter，统一格式 |
| hot path 日志性能影响 | 编译期过滤 + 异步队列 + 采样宏 |
| 日志泄漏敏感数据 | 提供 redaction 配置（field 名匹配正则即打 ***） |

## 后续可能演进

- 如果 spdlog 性能/功能远超自研 → 引入 spdlog（评估后单独 ADR）
- 如果 OpenTelemetry Logs 标准成熟 → 适配 OTel API
- 如果需要日志搜索 → 集成 ClickHouse / Loki
- 如果需要分布式追踪 → 集成 Jaeger / Tempo

## 参考

- spdlog: https://github.com/gabime/spdlog
- folly Log: https://github.com/facebook/folly/blob/main/folly/logging/README.md
- OpenTelemetry Logs: https://opentelemetry.io/docs/specs/otel/logs/
- glog（反面案例，禁用）
