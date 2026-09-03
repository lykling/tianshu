# ADR-0029：SLA 编译 v0 —— 加载期端到端 deadline 验证与预算分配

- **状态**：已接受
- **日期**：2026-09-03
- **决策者**：Pride Leong
- **关联**：[adr/0001](./0001-dsl-form.md) · [adr/0021](./0021-dsl-v0.md) · [adr/0019](./0019-coroutine-strategy.md) · [adr/0020](./0020-message-reflection-monitor.md) · [adr/0026](./0026-slice-input-model.md) · [adr/0027](./0027-state-as-data-channel-taxonomy.md)

---

## 需求清单（评审输入，逐条锁定）

| # | 需求 | 来源 |
|---|---|---|
| 1 | SLA 硬约束：端到端 deadline 在**编译期（加载期）验证**，不可满足时**加载期报错**，不进运行时抖动 | README 核心特性表 · 00-overview |
| 2 | `with_sla()` 已在 DSL 预留为注解槽（v0 接受并忽略），本 ADR 定义其消费方 | ADR-0021 决策 5 |
| 3 | **运行时零开销**承诺不可破坏：SLA 分析只发生在加载期，运行期测量是可选的旁路 | README · 00-overview |
| 4 | 验证结果可观测：预算表与运行时表现要进 ti 家族（monitor / record） | ADR-0020 |
| 5 | 与 v0 执行模型一致：v0 是"每条链在其 source 理程内同步贯通"的单线程级联，分析模型必须对齐这个现实，不许假装有静态调度 | ADR-0021 决策 5 |

## 决策

### D1：SLA 声明面 —— 类型化 `Sla`，端点语义

`with_sla` 从 string 注解槽升级为类型化结构（0.x 语义版本内允许有损变更，ADR-0021 风险表已预留）：

```cpp
struct Sla {
    std::chrono::microseconds deadline{0};  // 端到端：源消息到达 → 本通道产出
};

FlowChain<T>& with_sla(Sla sla);            // 绑定在链当前末端通道上
```

- **端点语义**：deadline 属于一个**通道**（SLA endpoint），不是整个 flow。一条 flow 可以有多个 endpoint（不同分支不同 deadline）。
- v0 只收 `deadline`；`period` 由源（`source` 的 interval / DagConfig）提供，不在 SLA 里重复声明。
- 未标 `with_sla` 的分支不参与验证（与 v0 一样自由运行）。

### D2：WCET 与成本输入 —— 声明优先，测量后补

| 输入 | API / 配置 | 默认 | 未声明时 |
|---|---|---|---|
| 算子 WCET | `.with_wcet(std::chrono::microseconds)`（map/join/op 上） | — | 取 `sla.default_wcet_us`（默认 100µs）并在分析输出里**告警**（列表点名） |
| 每跳传输成本 | `sla.hop_cost_intra_us` 配置 | 20µs（保守值，实测 INTRA 同步派发 < 5µs） | — |

Phase 1 引入校准回路：record v2（ADR-0028）回放 → ti-info 提取实测 p99 → 反填 WCET 声明。声明值与实测值偏差过大（>3×）在分析输出里告警。

### D3：分析模型 —— 两层，诚实边界

v0 执行现实（ADR-0021）：链在源线程内同步级联、调度器是 CFS 上的普通线程。**在 CFS 上不存在硬实时证明**，v0 不假装有。分两层：

**层 1：链路确定性预算（deterministic core）**

对每个 SLA endpoint，从 trace 图回溯全部上游贡献路径（含 join/span 的汇聚）：

```
L(endpoint) = max over paths P:  Σ_{op ∈ P} C(op) + hops(P) · c_hop
```

这是"线程一旦获得 CPU 就必然占用的时间"，确定性成立。

**层 2：机器饱和度准入（utilization admission）**

```
U = Σ_flow  L(flow 最长链) / T(flow 源周期)   ≤   m · (1 − margin)
```

m = 执行器核数（配置 `sla.machine_cores`，默认 `std::thread::hardware_concurrency()`），margin 默认 0.2。这是 Liu-Layland 式的充分性准入检查：满足则级联线程的排队延迟有界，不满足则无法背书。

**升级路径（写进 ADR，不写进 v0 代码）**：Phase 2 L1 codegen 产出静态调度 + 固定优先级执行（ADR-0019 Phase 2 协程调度）后，层 2 升级为固定优先级 RTA（`R_i = C_i + Σ_{j∈hp(i)} ⌈R_i/T_j⌉ C_j`），那时才有硬 RT 语义。

### D4：预算分配 —— deadline 下行分摊

验证通过后，deadline 沿 DAG **下行分解**为每算子预算（供运行期看门狗与 monitor 展示）：

```
budget(op) = deadline(endpoint) × C(op) / Σ_{op' ∈ path} C(op')
```

按声明 WCET 比例分摊（v0 不做 slack 分配优化）。join 汇聚点取各分支预算的 min。产出物是**预算表**：`(channel, op) → planned_us`，加载期随图一起构建。

### D5：判定语义 —— fail fast，分级

| 判定 | 结果 |
|---|---|
| 层 1 超限（`L > deadline`） | **加载期 ERROR**，构建失败。错误信息列出：违规路径（通道序列）、逐算子 WCET、L 与 deadline 差值、以及"最值得削减的算子"（WCET 占比最大者） |
| 层 2 超限（`U > m(1−margin)`） | v0 **WARNING**（图仍可运行）；`sla.strict_utilization=true` 时升级为 ERROR |
| 全部通过 | 图构建成功，预算表生效 |

`with_fallback` 联动（SLA 失败自动选降级图）推迟到 Phase 2 运行时容错，本 ADR 不占用。

### D6：运行时防御（v0.5，可选旁路）

SLA endpoint 处按消息对比 `Message::timestamp_ns`（transport 已有字段）与本地时钟，维护：

- e2e 时延直方图（bucket 8 个，对数尺度）+ 违约计数
- 通过 ti monitor（ADR-0020 的 schema sidecar 通道）对外暴露：`{endpoint, planned_us, p50_us, p99_us, miss_count}`

不满足 SLA 的图本来就不该加载成功，所以这是**漂移检测**（机器过载、WCET 退化），不是 SLA 执行本身。测量代码在非 SLA endpoint 上零执行（与 D-需求 3 一致）。

### D7：管线位置与模块

- 执行点：`Flow::build()` 内，trace 汇编完成后、通道接线前（加载期一次性）。
- 新模块：`tianshu/include/tianshu/sla/sla_analyzer.h` + `src/sla_analyzer.cc`（分析器）、预算表挂在 FlowRuntime 图上。
- 这是六阶段编译（00-overview L114）中 **SLA 规划 pass 的 v0 降维实现**：输入 trace 图，输出判定 + 预算表；Phase 2 它被提升为完整 pass。

## 里程碑分期

| 阶段 | 内容 | 验收 |
|---|---|---|
| **v0** | D1–D5：类型化 SLA、WCET 声明、两层分析、预算分配、fail-fast | 见下"测试与验收" |
| **v0.5** | D6：运行时直方图 + monitor 暴露 | demo 里 p99 与计划预算同屏 |
| **Phase 1** | 校准回路：ti-info 从 record 提取实测 → WCET 反填建议 | 偏差告警路径可演示 |
| **Phase 2** | L1 静态调度 + 固定优先级 RTA + fallback 自动降级 | 硬 RT 语义声明升级 |

## 被否决的备选

| 备选 | 否决理由 |
|---|---|
| 只做运行时 SLA 监控（miss 计数 + 告警） | 丢掉框架身份特性：加载期验证正是与 Cyber RT/ROS 2 的代差；运行时才知道超时 = 传统中间件 |
| v0 直接上多核固定优先级 RTA | 模型与现实不符（CFS + 线程池级联），产出假精度数字比没有更糟；RTA 等 Phase 2 静态调度落地 |
| 只做 utilization 准入、不做链路预算 | 无法回答"这条链端到端最坏多长"，join 汇聚与长链正是 AD 场景的核心问题 |
| SLA 保持 string 配置（`with_sla("50ms")`） | 无类型检查、无单位、无法携带结构化字段；ADR-0021 预留槽位时就预期类型化 |
| deadline 声明在 flow 级而非端点级 | 一条 flow 多分支不同时效需求（感知链 vs 日志旁路）表达不了 |

## 测试与验收（v0）

1. **满足案例**：demo 链（source→map→join→endpoint，deadline 充足）构建成功，预算表各算子比例正确（`Σ budget = deadline`）。
2. **违规案例**：deadline 压到链和以下 → 构建抛错，错误信息含完整路径序列与差值。
3. **join 回溯**：双分支汇聚，L 取分支 max；预算在汇聚点取 min。
4. **饱和度**：构造 U > 阈值的 flow 集 → WARNING（`strict_utilization` 下 ERROR）。
5. **默认 WCET 告警**：未声明 `with_wcet` 的算子出现在告警列表。
6. **零开销回归**：无 SLA 标注的现有测试（262 CMake / 23 Bazel）全数不变绿转红。

## 风险与开放问题

- **hop cost 与 WCET 默认值的可信度**：v0 用保守常数；Phase 1 校准回路闭环前，分析结论是"工程准入"而非"证明"——文档措辞必须保持这个诚实边界。
- **多 flow 共享算子**（`from()` 复用组件）时 WCET 计入各路径还是分摊：v0 按路径重复计入（保守），Phase 1 重审。
- **`sla.machine_cores` 与实际部署环境不一致**：容器/隔离核未感知；Phase 2 接 OSAL 拓扑查询。
