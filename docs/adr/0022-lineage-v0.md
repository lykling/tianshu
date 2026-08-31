# ADR-0022：Lineage v0 —— 数据血缘的级联模型

- **状态**：已接受（v0.5 增补见文末：分支模型 + 每消费者邮箱）
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

## v0.5 增补（2026-08-28）：分支模型 + 每消费者邮箱

为支撑 DSL join 原语（ADR-0021 增补），模型与传输机制各演进一步，v0 的线性语义保持不变：

### 1. Lineage 分支模型（DAG 出处）

- `Lineage` 内部改为 `branches: [(root, hops)]*`；线性链 = 单分支，`describe()` 渲染与 v0 **逐字节相同**
- join 输出的血缘 = `merge(两个父分支集)`；后续 hop 追加到**每条**分支（融合后所有路径都经过该 stage）
- 渲染：`a#1 -> x#1 -> j#0 | b#5 -> j#0`（两条路径汇于 j#0）
- `root()/hops()` 单分支访问器保留（v0 API 面），空血缘安全

### 2. 旁路 FIFO → 每消费者邮箱（解除单消费者约束）

- 原方案 3 的单 FIFO 在多消费者下会互相偷取（弹出序错配）——ADR 风险表预言的第一项
- v0.5：每个 (stage, 输入通道) 一个 `LineageMailbox`；`publish_bytes` 把血缘**扇出拷贝**到该通道注册的所有邮箱，各 stage 按自己的消费序弹出
- 正确性约束从「单消费者每通道」放宽为「每 stage 单线程消费自己的邮箱」；发布端多线程安全（邮箱内锁）
- 代价：每消费者一份拷贝（Lineage 小值对象，邮箱深度有界 = 2×队列深度）

### 3. 反馈环策略（v0.5.1，随全链路 demo 落地）

反馈边（`map_to` 写回被 join 的通道）会让 merge 在环上反复并入"回到同源"的分支，若无策略将**指数膨胀**。策略：

- **按 root 去重**：merge 时同 root channel 的分支只保留一条——保留 **hops 更长者**（环上回来的分支更新鲜，代表了最近一次绕环路径）；丢弃陈旧副本
- **分支上限** `kMaxBranches = 8` 兜底
- 效果：闭环中分支数恒定（demo 实测稳定 4：radar/front、radar/rear、gnss、chassis），环的绕行轨迹以 hops 形式线性增长并可见（`~3#0 -> ~4#0 -> chassis#0 -> ~3#1 -> ...`）
- 已知边界：单分支 hops 随环迭代线性增长——超长运行需 Phase 2 的"深度上限截断（根保留）"（原风险表已有此项）

### 4. 仍未变的部分

- 同步级联（join 回调在 dispatch 线程内联执行）——多 source 并行由每 source 一线程提供
- in-band `Message.lineage_ptr` 仍是 Phase 2 演进项（跨进程/record 落盘时启用）
