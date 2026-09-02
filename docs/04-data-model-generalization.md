# 04 · 数据模型一般化：切片输入、状态即数据、组件血缘、record 基底

> 状态：已实现（2026-08-31 落地，本文档同步补记）
> 关联：[adr/0025](./adr/0025-from-component-reference.md) · [adr/0026](./adr/0026-slice-input-model.md) · [adr/0027](./adr/0027-state-as-data-channel-taxonomy.md)
> 前置：[03 · 全链路闭环 Demo](./03-full-chain-demo.md)

---

## 概述

2026-08-31 评审驱动的数据模型四维一般化，分五个可独立验证的里程碑交付：

```
M0 共享地基（区间血缘 + 通道历史环）
  ├→ M1 状态即数据 + 恢复协议        （ADR-0027）
  ├→ M2 切片输入 + 雷达×IMU          （ADR-0026 A+B）
  ├→ ② 组件血缘接通                  （ADR-0025 修正）
  └→ ① record 基底                   （ADR-0026-C）
```

## M0：区间血缘 + 通道历史环

### 区间血缘（range hop）

`LineageHop` 增加 `seq_end` 字段——`ch#lo..#hi` 编码一条通道上**连续的一段消息**作为血缘贡献：

```
之前：20 条 IMU 消息 = 20 个独立分支（撑爆 kMaxBranches）
之后：20 条 IMU 消息 = 1 个区间跳 imu#102..#121（分支数恒定，成员表按 (channel, [lo,hi]) 可查）
```

单条消息渲染不变（`seq_end <= seq` 时省略），向后兼容。

### 通道历史环（HistoryRing）

`FlowRuntime` 在 `publish_bytes` 时把每条消息的 `(seq, bytes, lineage)` 捕获到各通道的有界环（默认 64 深）。**这是切片查询、状态恢复、monitor 观测、record 回放的统一读源**——不管环是活流填的还是回放填的。

## M1：状态即数据 + 恢复协议（ADR-0027）

### stateful 原语

```cpp
builder.stateful<TOut, TState>(chain, "out", "acc", impl);
// impl: on_init(OpPub<TOut>&, OpPub<TState>&)
//       handle(const TIn&, OpPub<TOut>&, OpPub<TState>&)
```

双 pub 共享同一父血缘——每次 handle 调用产出一版状态消息，血缘精确指向触发它的那条输入：

```
状态版本链（实测）：
  ev#0 -> acc#0     ← 第 0 条输入派生第 0 版状态
  ev#1 -> acc#1
  ...
```

### 恢复协议（EXACT MATCH 验证）

```
1. 取历史环中任一 checkpoint state#k
2. 读其血缘分支根 → 得知已吸收输入到 ev#N
3. 重放 ev#N+1..end 到恢复的算子（初始状态 = checkpoint 值）
4. 最终状态 == 完整运行状态（确定性验证）
```

demo（`state_recovery_demo`）：
```
[reference] absorbed 119 inputs, sum=476.0
[crash] recovering from checkpoint state#55 (n=56) — lineage says inputs absorbed through ev#55
[replay] 63 inputs re-published through the recovered op
[result ] recovered n=119 sum=476.0   reference n=119 sum=476.0   EXACT MATCH
```

## M2：切片输入（ADR-0026 A+B）

### span_join 原语

```cpp
builder.span_join<CompMsg>(lidar_chain, imu_chain, span_fn, time_fn, impl);
// 每条触发消息到达时：
//   [t0, t1] = span_fn(trig)
//   slice = history(data_ch) 中 time(msg) ∈ [t0, t1] 的全部消息（框架物化）
//   output = impl(trig, slice)
//   血缘 = trigger 分支 + 数据通道的区间跳分支
```

`Slice<T>` 是框架物化的成员视图（items + seq_lo/seq_hi + truncated 标记）——**切片由框架代取，父集合确定可知**，这就是切片输入和精确血缘天然相容的原因（ADR-0026 核心）。

### demo（`lidar_imu_demo`）：雷达 × IMU 运动补偿

AllLatest（各取最新一条）表达不了"一帧雷达需要扫掠区间内全部 IMU"——切片的原生场景：

```
lineage: mm/lidar#0 -> mm/~0#0 | mm/imu#1..#19 -> mm/~0#0
lineage: mm/lidar#1 -> mm/~1#1 | mm/imu#21..#39 -> mm/~0#1
...
9 frames, avg 19.0 IMU samples per frame
```

## ② 组件血缘接通（ADR-0025 修正落地）

修正前的边界：注册组件输出 = rooted（组件内部不透明）→ 反馈环在组件处截断。

修正后的链路：

```
ComponentBase::set_input_lineage_provider    ← from() 桥装邮箱（FIFO 1:1 配对）
  ↓
run_proc: refresh_input_lineage() → proc(msg) → publish 携带父血缘
  ↓
WriterBase::write(data, size, lineage_ptr)   ← 三参重载
  ↓
IntraWriter::write → Message.lineage_ptr     ← 经传输层 Message 携带
  ↓
泵回 reader：父非空 → publish_derived        ← 派生；空 → rooted（同 publish_op 规则）
```

全链 demo 血缘最终形态（环跨组件边界展开）：
```
radar/front#0 -> ~0#0 -> ~1#0 -> ~2#0 -> ~3#0 -> ~4#0 -> chassis#1 -> ~3#1 -> ~4#1 -> chassis#2 -> ...
```

降级边界精确对齐 ADR-0025 修正：**同步派生携带（proc 栈内）vs 异步发布 root（攒批/跨线程）**——不是"组件 vs 内联"。

## ① Record 基底（ADR-0026-C）

### 设计定位

**不是"录制功能"**——是切片查询 API 的离线基底。record 文件 = 通道历史环的持久化形态；回放 = 把文件灌回运行时、图重新跑、血缘由级联重建。

```
在线：通道历史环（内存） ←── 同一 API（span_join / 恢复 / monitor）──→ 离线：record 文件
```

### 数据包格式

```
┌──────────────────────────────┐
│ u64 magic 'TREC0001'         │
│ u32 record_count             │
├──────────────────────────────┤ × count
│ u32 channel_len              │
│     channel bytes            │  如 "avp/radar/front"
│ u64 seq                      │  通道本地序号
│ u32 payload_len              │
│     payload bytes            │  原始消息字节
│ u32 lineage_len              │
│     lineage describe string  │  仅供工具查看（回放不读）
└──────────────────────────────┘
```

| 决策 | 理由 |
|---|---|
| 回放不读文件里的血缘 | 血缘由重发布级联**重建**——活跑和回放血缘语义完全一致 |
| 只回放源通道消息 | 中间通道经活图级联重算——"离线=在线"的本质 |
| lineage 字符串写入 | 给 ti-monitor 等工具离线看 |
| v0 单次写盘 | 简单；流式追加是后续演进 |

### demo（`record_replay_demo`）

```
[live]   39 outputs, record saved=yes
[file]   78 messages loaded
[replay] 39 outputs
[result] 39/39 outputs bit-identical    ← 离线 == 在线
[lineage] live/tick#38 -> live/~0#38   ← 血缘由级联重建
```

## 运行方式

```bash
./build/<preset>/bin/state_recovery_demo   # M1：状态恢复 EXACT MATCH
./build/<preset>/bin/lidar_imu_demo        # M2：切片输入 + 区间血缘
./build/<preset>/bin/record_replay_demo    # ①：离线 == 在线
./build/<preset>/bin/full_chain_demo       # 全链闭环（含 ② 组件血缘环展开）
```

## 测试与质量

| 里程碑 | 新增测试 |
|---|---|
| M0 | 区间渲染/合并、历史环有界性/字节往返/absent |
| M1 | 状态版本数 + 精确血缘串、describe 渲染、恢复确定性 |
| M2 | 切片物化 + 区间血缘串、空切片省略分支 |
| ② | 组件输出派生血缘 + 环展开 + 有界增长 |
| 全局 | 253/253 CMake（GCC+Clang）· 22/22 Bazel · tidy 零警告 |
| 覆盖率 | lineage.cc + dsl_runtime.cc 行覆盖 94.8% / 函数 100% |
