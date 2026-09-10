# ADR-0031：降级阶梯 —— `with_fallback` 声明与运行期降级信号

- **状态**：已接受
- **日期**：2026-09-10
- **决策者**：Pride Leong
- **关联**：[adr/0001](./0001-dsl-form.md) · [adr/0029](./0029-sla-compilation.md) · [adr/0030](./0030-l1-compiler.md)

---

## 背景

ADR-0001 的目标 DSL 形态里有 `.with_fallback("perception_flow_lite")`：一个流在
持续无法满足 SLA 时，应能降级到另一个更轻的注册流。截至本 ADR 之前，仓库里
已有的件是：SLA 加载期验证与预算分配（ADR-0029 v0）、运行期 e2e 直方图与
miss 计数器（ADR-0029 v0.5，`sla_stats.h`）、注册流表
（ADR-0030 M-D，`REGISTER_TRACEABLE_FLOW`）。缺的是把它们连成"降级阶梯"的
声明动词与运行期判定。

## 决策

### D1：v0 语义 —— 声明 + 加载期校验 + 运行期降级信号（不做热切换）

分四层落地，每层独立可测：

1. **DSL 声明**：`FlowChain<T>::with_fallback(name)` / `FlowBuilder::with_fallback(name)`。
   `build()` 时 fail-fast 校验：目标名必须已在注册表中，且不允许自引用。跨流
   引用与 `from()` 一样是加载期锁定的契约（与 ADR-0029 加载期验证哲学一致）。
2. **IR 携带**：`IrGraph::fallback_flow()`；`export_conf()` 输出
   `fallback_flow = "..."`，产物与声明图一致可溯源。
3. **运行期检测**：`FlowRuntime::run_for` 在声明了 fallback 且有 SLA 端点时
   启动 watcher 线程，每 20ms 采样一次 miss 计数器；任一端点单窗口内 miss
   增量 ≥1 即记一次降级事件。事件暴露为 `FlowRuntime::fallback_state()`
   （声明名、事件数、最后违规端点与其 miss 计数）。
4. **可观测不接管**：v0 只产生信号（计数器 + 状态查询），**不**热切换到
   fallback 流。热切换（停源、排空、按名重建 fallback 流的 runtime）是 v1
   的范围，需要 ADR-0026 的 quiesce 语义配合，单独评审。

### D2：为什么 v0 不做热切换

- 热切换的正确性依赖"排空再重建"：在线 cascade 中途换流，lineage 连续性与
  状态恢复（ADR-0026/0027）都要新协议，风险集中在一次交付里不可控。
- 信号本身已闭环可用：ti monitor / 测试可以消费 `fallback_state()` 驱动
  外部决策（进程级重启到 lite 流、告警、记录），这正是车端常见的部署形态。
- 与 SLA 的 staging 对齐（v0 加载期 → v0.5 运行期直方图 → 本 ADR v0 信号
  → 未来 v1 切换），每步都有独立验证面。

### D3：判定阈值

窗口 20ms、单窗口 miss 增量 ≥1 触发。理由：miss 计数器本身已是"越过
deadline"的强信号，窗口化只为去抖（单次抖动不跨窗口重复计数）。阈值是
编译期常量（`kWindow`/`kWindowMisses`），调参需求出现时再提升为 DSL
配置项，避免过早配置面。

## 后果

- `Flow`/`IrGraph`/`.conf` 产物新增 `fallback_flow` 字段（旧产物无此字段，
  解析侧向后兼容——字段缺失即"无 fallback"）。
- watcher 线程仅在声明 fallback 且有 SLA 端点时存在，无 fallback 的流零
  新增开销（与 ADR-0029 D6"未声明零开销"一致）。
- `flow.h` 的注册表探测 `detail::is_registered_flow` 同时服务 build 校验与
  M-D dry-run，注册表仍是无锁侵入式链（静态初始化）。
- 明确不做：fallback 链循环检测（A→B→A）。注册表按名校验已排除未知名与
  自引用；跨流环在 v1 热切换时才有运行期意义，届时一并处理。
