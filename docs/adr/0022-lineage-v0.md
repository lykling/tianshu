# ADR-0022：Lineage v0 —— 数据血缘的级联模型

- **状态**：已接受
- **日期**：2026-08-27
- **决策者**：Pride Leong
- **关联**：[adr/0021](./0021-dsl-v0.md) · [adr/0001](./0001-dsl-form.md) · [adr/0002](./0002-cyber-relation.md)

---

## 背景

ADR-0001 的核心卖点之一是「自动 trace」：声明式图里每条数据能回答**「我是谁产生的、经过了哪些算子」**。这是 L2 血缘与容错（重放、审计、故障定位）的地基。v0 需要在 DSL 解释器上把模型立起来，且不能给热路径加显著开销。

## 候选方案

### 方案 1：消息内嵌血缘（in-band）

血缘字段直接放进消息结构（头部或尾随），随 payload 一起走传输。

**否决（v0）**：侵入所有消息格式（POD 布局被打破、protobuf/fbs 要加字段）；跨进程 SHM 时指针序列化复杂。Phase 2 经 L4-TRANS `Message.lineage_ptr` 演进时再评估。

### 方案 2：全局血缘日志（out-of-band，按时间戳对齐）

每级算子把 (输入 seq, 输出 seq) 写全局日志，事后按时间对齐重建。

**否决**：对齐是概率性的（时钟、并发），重建成本高，实时查询（monitor 里看单条消息链）做不到。

### 方案 3：旁路 FIFO 血缘（side-car，**已选**）

每条消息的血缘作为 `Lineage` 值对象，由 DSL 运行时在**发布前**推入该通道的旁路 FIFO；下游消费消息时弹出对应血缘。v0 约束下单写单读，FIFO 序 = 消息序，零错配。

## 决策

**方案 3**，模型如下：

### Lineage 值对象

```
root: (源通道, 源 seq)            —— 消息的出身
hops: [(通道, seq)]*              —— 级联经过的每一跳的中间消息标识
```

- `source` 发布时 `Lineage::rooted(channel, tick)`
- 每级 `map` 发布输出时追加一跳：`(输出通道, 该通道上的输出 seq)`
- `sink` 收到 `(值, 血缘)`，血缘链完整回答「从哪来、经过谁」

describe 形如：`demo/imu#7 -> demo/~0#7 -> demo/~1#7`（每跳标识该通道上的第几条消息，全局可定位）。

### 正确性依赖（v0 约束，ADR-0021 已同步）

- **单消费者**：每通道至多一个下游（链式 DSL 天然满足）——FIFO 弹出序才与消息消费序对齐
- **发布前推入**：血缘先于 dispatch 入队，回调触发时必可弹出
- **同步级联**：v0 无并发写同一通道；多 source 并行时各通道独立，互不干扰

### API 面

- `Lineage::rooted / add_hop / root / hops / describe`——纯值类型，可拷贝可序列化（Phase 2 进 SHM/record）
- DSL 用户不接触 Lineage API：级联全自动化；sink 签名 `(const T&, const Lineage&)` 是唯一可见点

## 影响范围

- 新增 `core/lineage.h/.cc`（值对象）+ DSL 运行时旁路 FIFO（`dsl_runtime.h` 的 `side_lineage_`）
- demo：sink 打印血缘链（与 ADR-0021 demo 合一，`examples/dsl_demo.cc`）
- 功能点：L2-LIN-1..2 的 v0 子集进开发计划
- 演进路径：Phase 2 `Message.lineage_ptr` 携带 → record 落盘 → L2 血缘图查询；`hops` 改紧凑编码（通道 ID 表 + 定长记录）

## 风险与缓解

| 风险 | 缓解 |
|---|---|
| 旁路 FIFO 与消息错配（未来多消费者） | v0 静态约束（DSL 链式）；多消费者引入时切 in-band 或按 (seq) 索引的 map |
| 血缘体积随链长增长 | v0 链短（<10 跳）；Phase 2 通道 ID 压缩 + 深度上限截断（根保留） |
| 忘记弹/多弹 | FIFO 与 visitor 缓冲同深度上限；测试断言链内容完整覆盖 |
