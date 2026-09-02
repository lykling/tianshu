# ADR-0026：数据切片输入模型 —— 触发器 + 有界历史查询，多父血缘

- **状态**：已接受（Phase A+B+C 全部落地）
- **日期**：2026-08-31
- **决策者**：Pride Leong
- **关联**：[adr/0021](./0021-dsl-v0.md) · [adr/0022](./0022-lineage-v0.md) · [adr/0024](./0024-dsl-op-primitive.md) · [adr/0025](./0025-from-component-reference.md)

---

## 背景

当前全部消费原语是**点消费**：`proc(const M&)` / `handle(const T&, OpPub&)` / `DataVisitor::try_fetch_0()`——一次一条。但真实数据流的输入形态常常是**一片**：

- 雷达点云运动补偿：一帧 10Hz 雷达需要扫掠区间内的**全部** 200Hz IMU 消息（~20 条）
- 滑动窗口估计：尾部 N 条 / 尾部 100ms
- 触发对齐融合：按触发消息的时间戳取 `[t0, t1]` 区间

`AllLatest`（各取最新一条）表达不了这些。本 ADR 将消费契约从"单条推送"升级为"**触发器 + 对通道有界历史的切片查询**"，并让血缘天然承载多父集合。

## 决策

### 1. 统一原理：stage = trigger + fetch

现有原语重新表述为退化形态：

| 现有原语 | 等价的 trigger + fetch |
|---|---|
| `map(a)` | `trigger(a)` + `fetch(latest(a))` |
| `join(a,b)` AllLatest | `trigger(a∧b 皆备)` + `fetch(latest(a), latest(b))` |
| 窗口融合（新） | `trigger(lidar)` + `fetch(span(imu, t0, t1))` |

目标 API 形态：

```cpp
builder.stage<PointCloudOut>("compensate")
    .trigger(lidar_chain)
    .fetch(latest<PoseMsg>("pose"),
           span<ImuMsg>("imu", trig.t0, trig.t1),   // 触发对齐区间
           last_n<ImuMsg>("imu", 4),
           window<ImuMsg>("imu", 100ms))
    .impl([](const Slice<PoseMsg>&, const Slice<ImuMsg>& imu) { /* 区间视图 */ });
```

`Slice<T>` 是区间视图（begin/end/size/随机访问），底层是各通道的**有界历史环**（CacheBuffer 既有语义），切片在触发时由**框架物化**。

### 2. 血缘：切片 = 多父集合，精确且紧凑

- 切片输出的血缘 = 切片内每条消息都是父：多父 merge（branch/merge/root 去重模型直接承接，v0.5 地基）
- **区间跳**：同通道连续成员压缩为一个贡献——`imu#102..#121`——20 条父不撑爆分支数；完整成员表按 `(channel, seq-range)` 从运行时索引可查
- 示例：`lidar#7 -> fus#k | pose#9 -> fus#k | imu#102..#121 -> fus#k`

**与 ADR-0025 修正案的衔接**：切片由框架物化 → 父集合确定可知 → **批量输入与精确血缘天然相容**。不可知的只有"用户绕过框架自己攒消息"（那是 ADR-0025 定的真正降级点），框架代取的一片数据每条都有据可查。

### 3. 离线统一：同一 API，两个基底

切片查询面对的底层不必是活流：

- 在线：通道的有界内存历史环
- 离线：record 文件——"给我 `[t0,t1]` 的 imu" 就是回放的天然 API

**同一份 stage 声明指向内存环（在线）或 record（离线/回放/重放调试）**。在线/离线不是两套代码，是切片查询的两个基底。这是"数据驱动"的完整含义，也是 L2 record/replay 的 API 基础。

### 4. 与既有机制的关系

| 既有机制 | 在本模型中的位置 |
|---|---|
| CacheBuffer（通道有界环） | 切片的存储基底（已存在） |
| DataVisitor `on_fuse` | 触发器机制的雏形（已存在） |
| Lineage branch/merge/root 去重 | 多父承接（已存在），新增区间跳编码 |
| map / join / op | 退化形态，保留为语法糖（不破坏现有 API） |
| MonitorBuffer 暂停浏览 | 切片浏览的先行实践 |

## 实现分期

- **A（切片原语）**：`last_n` / `window` / `span` fetch + Slice 视图 + 多父血缘（含区间跳）；DSL runtime 内实现
- **B（典型验证）**：雷达 × IMU 运动补偿 demo——span 触发对齐 + `imu#n..m` 区间血缘断言
- **C（离线基底）**：~~record 落盘/回放接切片查询~~ **已落地（2026-08-31）**——RecordFile + record_to/replay_from，离线 == 在线 39/39 bit-identical

## 风险与缓解

| 风险 | 缓解 |
|---|---|
| 历史环深度与切片需求不匹配（要的比留的多） | fetch 声明携带最小深度需求，装配时校验/调整环深；不足时切片截断并标记 |
| 区间血缘压缩掩盖成员级差异 | 完整成员表可查（运行时索引）；describe 的区间跳可展开 |
| 触发风暴（高频触发 × 大切片） | 切片物化在触发线程内同步完成（与同步级联一致）；超频可配合并策略（v1 从简：逐触发） |
| API 面膨胀 | map/join 保留为语法糖；stage 形态作为一般化出口，文档以退化关系引导 |
