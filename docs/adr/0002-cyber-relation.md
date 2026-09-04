# ADR-0002：与 Cyber RT 的关系

- **状态**：已接受
- **日期**：2026-08-10
- **决策者**：Pride Leong
- **关联**：[00-overview.md](../00-overview.md)

## 背景

天枢要落地为工程实现，必须决定与 Apollo Cyber RT 的代码关系。这影响：

- 仓库结构
- 工作量
- 上游同步成本
- 开源可行性
- 用户迁移成本

## 候选方案

### 方案 A1：独立实现 + cyber 作为可选 transport 后端

天枢的核心层独立实现，但 transport 层通过 adapter 接入多种后端：cyber / ROS 2 / ZeroMQ / zenoh / 自研 SHM。

**优点**：起步快，复用 cyber 成熟 transport；多后端灵活。
**缺点**：仍然依赖 cyber 二进制（许可证风险）；adapter 抽象层有性能损耗；多后端测试矩阵爆炸。

### 方案 A2：完全重写，API 兼容（**已选**）

天枢完全从零实现所有层（含 transport / 协程 / 调度器），不引用 cyber 任何代码。API 风格向 cyber 兼容（`Node::CreateReader`、`Writer<T>::Write`、`Component<M0, M1>` 等），目标是用户从 cyber 迁移时源代码改动最小化。

**优点**：完全解耦，零许可证风险，零上游同步成本，可独立开源。
**缺点**：工作量最大，需要重写 transport / 协程 / 调度器等基础设施。

### 方案 B：fork cyber 原地改造

fork apollo-lite/cyber 到 tianshu/cyber，在 fork 上做改造。

**优点**：起步最快，复用全部现有代码。
**缺点**：与上游 cyber 强耦合，未来同步 Apollo 主干成本指数增长；绑定 Apollo Apache 2.0 + 特殊条款许可证；架构改造受现有代码约束。

### 方案 C：cyber 之上的库（动态加载）

tianshu 作为链接到 libcyber.so 的库，加载期生成 cyber DAG。

**优点**：工作量最小。
**缺点**：trace + codegen 严重依赖 cyber 内部 API（私有头文件），长期维护成本极高；性能优化受 cyber 内部实现约束。

## 决策

**选方案 A2**：完全重写，API 兼容 cyber。

## 决策依据

### 1. 工程解耦

- 零 Apollo 许可证风险（Apache 2.0 + Apollo 特殊条款 vs 天枢可独立以更宽松许可开源）
- 零上游同步成本（Apollo 主干每周都在动）
- 测试 / CI / 工具链完全自控
- 可以独立决定支持的架构（x86 / ARM / RISC-V）、支持的 OS（Linux / QNX / VxWorks）

### 2. 性能可控

- transport 是性能关键路径，自研才能保证 zero-overhead 承诺（H2 假设）
- 复用 cyber 的 transport 意味着性能天花板被 cyber 限制
- 自研可以针对 AD 场景深度优化（例如 GPU direct、NIC offload）

### 3. 用户迁移成本可控

- API 风格兼容 cyber → 用户迁移时主要改 `#include` 和 namespace
- 理想情况：源代码改动 < 10%（仅 import + 个别签名调整）
- 即使需要重写 transport，对**用户代码**是透明的（用户写的是 flow 函数，不直接调 transport API）

### 4. 工作量是真实成本，但可摊薄

| 模块 | 估算工作量 | 备注 |
|---|---|---|
| Transport（INTRA + SHM + 跨机） | 8-12 周 | 最大头；可分阶段（先 INTRA+SHM） |
| 协程（CRoutine） | 2-4 周 | 可起步用 `ucontext` / `boost.context` |
| 调度器（Classic + Choreography） | 3-5 周 | 两种策略 |
| Node / Reader / Writer / Component | 2-3 周 | API 兼容 cyber |
| Blocker / ObjectPool / CacheBuffer | 1-2 周 | 通用原语 |

总计：16-26 周（4-6 个月）的 transport + runtime 工作量。这是真实成本，但与 Phase 2 MVP 同期推进，不会独立阻塞主线。

### 5. 风险可控

- 协程起步用 `ucontext` 或 `boost.context`（无汇编），稳定后再做汇编优化
- Transport 起步只做 INTRA + SHM，跨机用第三方库（zenoh / Fast-DDS）作为过渡，后期自研替换
- API 兼容性可以通过"对比 cyber 单测"验证

## 影响

### 仓库结构

```
tianshu/
├── tianshu/
│   ├── core/              # Node / Reader / Writer / Component 等
│   ├── dsl/               # fluent builder（基于 core 之上）
│   ├── trace/             # RAII guard + trace 引擎
│   ├── compiler/          # 六阶段 pass 链
│   ├── codegen/           # C++ 源码生成器
│   ├── transport/         # INTRA / SHM / 跨机（完全自研）
│   ├── coroutine/         # CRoutine（ucontext / 汇编）
│   ├── scheduler/         # Classic / Choreography
│   ├── lineage/           # 血缘 + 容错
│   ├── sla/               # RTA + 调度推导
│   └── runtime/           # ti launch + 闭环
├── tests/
├── benchmarks/
├── examples/
├── tools/                 # tianshu-ctl
└── docs/
```

### API 兼容性合约

天枢公开 API 与 cyber 等价的部分（不完全列表）：

| Cyber RT API | 天枢等价 API | 兼容级别 |
|---|---|---|
| `apollo::cyber::Node` | `tianshu::Node` | 完全兼容（同名方法） |
| `Node::CreateReader<T>(channel)` | `Node::CreateReader<T>(channel)` | 完全兼容 |
| `Node::CreateWriter<T>(channel)` | `Node::CreateWriter<T>(channel)` | 完全兼容 |
| `Reader<T>::Observe()` | `Reader<T>::Observe()` | 完全兼容 |
| `Writer<T>::Write(msg)` | `Writer<T>::Write(msg)` | 完全兼容 |
| `Component<M0, M1, M2, M3>` | `Component<M0, M1, M2, M3>` | 兼容（但推荐用 flow 函数替代） |
| `TimerComponent` | `TimerComponent` | 兼容 |
| `cyber::Init(argc, argv)` | `tianshu::Init(argc, argv)` | 完全兼容 |
| `mainboard -d xxx.dag` | `ti launch -d xxx.dag` | 等价（ti 家族命名，见下） |
| `.dag` / `.conf` 格式 | 完全兼容 + 扩展字段 | 二进制兼容 |

迁移用户主要改动：

- `#include "cyber/cyber.h"` → `#include "tianshu/tianshu.h"`
- `namespace cyber` → `namespace tianshu`（或保留 cyber 别名做过渡）
- 个别内部 API（如 service_discovery）签名调整

### 测试策略

- 单元测试：对标 cyber 的单测覆盖（覆盖率不低于 cyber）
- 兼容性测试：跑 cyber 的官方 example（适配 namespace 后）确认输出一致
- 性能测试：5 类典型链路对比 cyber，H2 验证 < 1% 差距

### 工期影响

- Phase 1 PoC 多 4-6 周（实现 transport + 协程最小子集）
- Phase 2 MVP 多 4-6 周（完善 transport + 跨机）
- 总工期 +2-3 个月，但换来零许可证风险 + 零上游成本 + 性能可控

## 术语边界与工具命名（2026-08-26 增补）

L4 落地期间确定的两个命名决策，避免与 L1 编译层及工具生态混淆：

**Component 保留（L4 用户编写单元）**。cyber API 兼容是本 ADR 的核心承诺，
Component 正是 cyber 用户 API 的全部表面；C++ 语境下替代词均有硬伤
（`operator` 是关键字、`Kernel` 留给 GPU、`Process` 与跨进程 SHM 语境冲突、
`Node` 已被工厂占用）。分层术语明确为：

| 层 | 术语 | 含义 |
|---|---|---|
| 用户编写 | flow function（DSL）/ Component（C++） | 算子的两种编写模式 |
| L1 编译期 | Operator（IR 图节点） | 编译器词汇表，与 L4 无关 |
| L4 运行期 | Component 实例 | Launcher 从 DAG 配置实例化的产物 |

**CLI 家族：`ti` + `ti-*`（统一入口 + 独立工具，kubectl/docker 插件模式）**。
`ti` 是 ~40 行 dispatcher，按 PATH 查找并 exec `ti-<verb>`；工具全部是可独立
调用的 `ti-*` 二进制，新工具零成本进入统一入口。命名否决记录：`mainboard`
（cyber 历史包袱、硬件隐喻错位）、`ts`（与 moreutils 时间戳工具冲突，且实时
系统语境 ts 首先读作 timestamp）、`tsctl`/`tictl`（`ctl` 后缀与子命令动词
结构冲突）。Phase 1 落地 `ti` + `ti-launch`；`ti-ctl`（ADR-0012）、
`ti-console`（ADR-0014）按各自计划归队。

## 后续可能演进

- 如果 transport 自研在 Phase 2 严重滞后 → 临时引入 zenoh 或 Fast-DDS 作为过渡后端（保持 API 不变；但需通过 [adr/0005 依赖治理流程](./0005-lightweight-multiplatform.md) 审批）
- 如果用户迁移成本实测过高 → 提供 `cyber_compatible` namespace 别名 + 自动迁移工具（AST 改写）
- 如果未来 Apollo cyber 开放更友好的 License → 可以重新评估是否提供 cyber 后端作为可选项

## 参考

- Cyber RT 源码：https://github.com/ApolloAuto/apollo/tree/master/cyber
