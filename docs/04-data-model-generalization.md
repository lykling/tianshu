# 04 · 数据模型一般化：切片输入、状态即数据、组件血缘、record 基底

> 状态：已实现（2026-08-31 落地，record v2 于 2026-09-01 升级）
> 关联：[adr/0025](./adr/0025-from-component-reference.md) · [adr/0026](./adr/0026-slice-input-model.md) · [adr/0027](./adr/0027-state-as-data-channel-taxonomy.md) · [adr/0028](./adr/0028-record-format-v1.md)
> 前置：[03 · 全链路闭环 Demo](./03-full-chain-demo.md)

---

## 概述

2026-08-31 评审驱动的数据模型四维一般化，分五个可独立验证的里程碑交付：

```
M0 共享地基（区间血缘 + 通道历史环）
  ├→ M1 状态即数据 + 恢复协议        （ADR-0027）
  ├→ M2 切片输入 + 雷达×IMU          （ADR-0026 A+B）
  ├→ ② 组件血缘接通                  （ADR-0025 修正）
  └→ ① record 基底 v2               （ADR-0028，2026-09-01 升级）
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

双 pub 共享同一父血缘——每次 handle 调用产出一版状态消息，血缘精确指向触发它的那条输入。

<svg viewBox="0 0 880 320" xmlns="http://www.w3.org/2000/svg" font-family="sans-serif">
  <defs>
    <marker id="a1" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6" orient="auto">
      <path d="M0,0 L10,5 L0,10 z" fill="#37474f"/>
    </marker>
  </defs>
  <rect x="20" y="120" width="120" height="44" rx="8" fill="#e8f5e9" stroke="#2e7d32"/>
  <text x="80" y="138" text-anchor="middle" font-size="13">source ev</text>
  <text x="80" y="154" text-anchor="middle" font-size="10" fill="#5f6368">每 5ms 一条</text>
  <rect x="200" y="80" width="180" height="120" rx="12" fill="#fff3e0" stroke="#ef6c00" stroke-width="1.5"/>
  <text x="290" y="105" text-anchor="middle" font-size="14" font-weight="bold">stateful</text>
  <text x="290" y="125" text-anchor="middle" font-size="11" fill="#5f6368">handle(ev, out_pub, state_pub)</text>
  <rect x="220" y="140" width="60" height="28" rx="6" fill="#e3f2fd" stroke="#1565c0"/>
  <text x="250" y="158" text-anchor="middle" font-size="10">out_pub</text>
  <rect x="300" y="140" width="60" height="28" rx="6" fill="#f3e5f5" stroke="#7b1fa2"/>
  <text x="330" y="158" text-anchor="middle" font-size="10">state_pub</text>
  <text x="290" y="190" text-anchor="middle" font-size="10" fill="#ef6c00">双 pub 共享同一父血缘</text>
  <line x1="140" y1="140" x2="198" y2="140" stroke="#37474f" stroke-width="1.5" marker-end="url(#a1)"/>
  <line x1="280" y1="168" x2="280" y2="230" stroke="#1565c0" stroke-width="1.5" marker-end="url(#a1)"/>
  <rect x="220" y="230" width="120" height="30" rx="6" fill="#e3f2fd" stroke="#1565c0"/>
  <text x="280" y="249" text-anchor="middle" font-size="11">out 通道（输出）</text>
  <line x1="360" y1="168" x2="440" y2="168" stroke="#7b1fa2" stroke-width="1.5" marker-end="url(#a1)"/>
  <rect x="440" y="168" width="120" height="30" rx="6" fill="#f3e5f5" stroke="#7b1fa2"/>
  <text x="500" y="186" text-anchor="middle" font-size="11">state 通道（版本链）</text>
  <rect x="600" y="60" width="260" height="200" rx="10" fill="#fafafa" stroke="#e0e0e0"/>
  <text x="730" y="85" text-anchor="middle" font-size="12" fill="#5f6368">状态版本链示例</text>
  <text x="620" y="110" font-size="10" font-family="monospace" fill="#7b1fa2">ev#0 → acc#0</text>
  <text x="620" y="128" font-size="10" font-family="monospace" fill="#7b1fa2">ev#1 → acc#1</text>
  <text x="620" y="146" font-size="10" font-family="monospace" fill="#7b1fa2">ev#2 → acc#2</text>
  <text x="620" y="164" font-size="10" font-family="monospace" fill="#7b1fa2">...</text>
  <text x="620" y="182" font-size="10" font-family="monospace" fill="#7b1fa2">ev#58 → acc#58</text>
  <text x="620" y="200" font-size="10" font-family="monospace" fill="#7b1fa2">ev#59 → acc#59 ← checkpoint</text>
  <line x1="620" y1="206" x2="850" y2="206" stroke="#c62828" stroke-width="1" stroke-dasharray="4 3"/>
  <text x="620" y="222" font-size="10" fill="#c62828">↑ 恢复基点：血缘指出吸收至 ev#59</text>
  <text x="620" y="240" font-size="10" fill="#c62828">→ 只重放 ev#60..ev#119</text>
</svg>

### 恢复协议（EXACT MATCH 验证）

```
1. 取历史环中任一 checkpoint state#k
2. 读其血缘分支根 → 得知已吸收输入到 ev#N
3. 重放 ev#N+1..end 到恢复的算子（初始状态 = checkpoint 值）
4. 最终状态 == 完整运行状态（确定性验证）
```

demo 实测（`state_recovery_demo`）：
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

<svg viewBox="0 0 880 300" xmlns="http://www.w3.org/2000/svg" font-family="sans-serif">
  <defs>
    <marker id="a2" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6" orient="auto">
      <path d="M0,0 L10,5 L0,10 z" fill="#37474f"/>
    </marker>
  </defs>
  <rect x="20" y="60" width="140" height="44" rx="8" fill="#e8f5e9" stroke="#2e7d32"/>
  <text x="90" y="78" text-anchor="middle" font-size="13">lidar (10Hz)</text>
  <text x="90" y="94" text-anchor="middle" font-size="10" fill="#5f6368">触发源</text>
  <rect x="20" y="140" width="140" height="44" rx="8" fill="#e8f5e9" stroke="#2e7d32"/>
  <text x="90" y="158" text-anchor="middle" font-size="13">imu (200Hz)</text>
  <text x="90" y="174" text-anchor="middle" font-size="10" fill="#5f6368">数据通道</text>
  <rect x="220" y="80" width="200" height="90" rx="12" fill="#fff3e0" stroke="#ef6c00" stroke-width="1.5"/>
  <text x="320" y="108" text-anchor="middle" font-size="14" font-weight="bold">span_join</text>
  <text x="320" y="128" text-anchor="middle" font-size="10" fill="#5f6368">span_fn(trig) → [t0, t1]</text>
  <text x="320" y="144" text-anchor="middle" font-size="10" fill="#5f6368">slice = history 中 time ∈ [t0,t1]</text>
  <text x="320" y="158" text-anchor="middle" font-size="10" fill="#5f6368">框架物化成员视图</text>
  <line x1="160" y1="82" x2="218" y2="110" stroke="#37474f" stroke-width="1.5" marker-end="url(#a2)"/>
  <line x1="160" y1="162" x2="218" y2="140" stroke="#37474f" stroke-width="1.5" marker-end="url(#a2)"/>
  <rect x="220" y="200" width="200" height="60" rx="8" fill="#fce4ec" stroke="#c62828" stroke-dasharray="5 3"/>
  <text x="320" y="222" text-anchor="middle" font-size="11" fill="#c62828">HistoryRing（有界历史环）</text>
  <text x="320" y="240" text-anchor="middle" font-size="9" fill="#c62828">(seq, bytes, lineage) × 64</text>
  <line x1="320" y1="170" x2="320" y2="198" stroke="#c62828" stroke-width="1" stroke-dasharray="3 3"/>
  <line x1="420" y1="120" x2="480" y2="120" stroke="#37474f" stroke-width="1.5" marker-end="url(#a2)"/>
  <rect x="480" y="100" width="140" height="40" rx="8" fill="#e3f2fd" stroke="#1565c0"/>
  <text x="550" y="118" text-anchor="middle" font-size="12">comp 输出</text>
  <text x="550" y="132" text-anchor="middle" font-size="9" fill="#5f6368">雷达点云补偿</text>
  <rect x="650" y="60" width="220" height="200" rx="10" fill="#fafafa" stroke="#e0e0e0"/>
  <text x="760" y="85" text-anchor="middle" font-size="12" fill="#5f6368">血缘示例（实测）</text>
  <text x="665" y="110" font-size="9" font-family="monospace" fill="#37474f">lidar#0 -> comp#0</text>
  <text x="665" y="122" font-size="9" font-family="monospace" fill="#37474f">  | imu#1..#19 -> comp#0</text>
  <text x="665" y="134" font-size="9" font-family="monospace" fill="#37474f">  ↑ 区间跳：19 个父</text>
  <text x="665" y="146" font-size="9" font-family="monospace" fill="#37474f">    压缩为 1 个分支</text>
  <line x1="665" y1="158" x2="855" y2="158" stroke="#e0e0e0"/>
  <text x="665" y="178" font-size="9" font-family="monospace" fill="#37474f">lidar#1 -> comp#1</text>
  <text x="665" y="190" font-size="9" font-family="monospace" fill="#37474f">  | imu#21..#39 -> comp#1</text>
  <text x="665" y="210" font-size="10" fill="#1565c0">avg 19.0 IMU / lidar 帧</text>
  <text x="665" y="228" font-size="10" fill="#1565c0">AllLatest 表达不了此形态</text>
</svg>

`Slice<T>` 是框架物化的成员视图（items + seq_lo/seq_hi + truncated 标记）——**切片由框架代取，父集合确定可知**，这就是切片输入和精确血缘天然相容的原因。

### demo（`lidar_imu_demo`）实测

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
泵回 reader：父非空 → publish_derived        ← 派生；空 → rooted
```

全链 demo 血缘最终形态（环跨组件边界展开）：
```
radar/front#0 -> ~0#0 -> ~1#0 -> ~2#0 -> ~3#0 -> ~4#0 -> chassis#1 -> ~3#1 -> ~4#1 -> chassis#2 -> ...
```

降级边界精确对齐 ADR-0025 修正：**同步派生携带（proc 栈内）vs 异步发布 root（攒批/跨线程）**。

## ① Record 基底 v2（ADR-0028，2026-09-01 升级）

### 设计定位

**不是"录制功能"**——是切片查询 API 的离线基底。同一个 `span_join`/恢复/monitor 读源（HistoryRing），不管是活流填的还是回放填的。

### v2 格式结构（取代 v0 dump）

```
┌───────────────────────────────────────────────────────────────────┐
│ FILE HEADER（128B）                                              │
│   magic / version / flags / CRC / offsets / compression          │
├───────────────────────────────────────────────────────────────────┤
│ CHANNEL DICTIONARY                                                │
│   name → compact ID + format + type_name + schema blob            │
│   （ADR-0020 字段表内嵌：离线工具零预配置）                       │
├───────────────────────────────────────────────────────────────────┤
│ DATA CHUNK × M                                                    │
│   LZ4 / ZSTD / None 压缩 · 8-byte 对齐 · CRC · 时间戳排序        │
│   MESSAGE ENTRY：channel_id + seq + ts_ns + payload + lineage    │
├───────────────────────────────────────────────────────────────────┤
│ CHUNK INDEX（随机访问：offset / channels / ts 范围）              │
├───────────────────────────────────────────────────────────────────┤
│ STATISTICS（per-channel count / bytes / seq / ts / avg-rate）    │
├───────────────────────────────────────────────────────────────────┤
│ FILE FOOTER（64B）                                                │
└───────────────────────────────────────────────────────────────────┘
```

<svg viewBox="0 0 880 360" xmlns="http://www.w3.org/2000/svg" font-family="sans-serif">
  <defs>
    <marker id="a3" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6" orient="auto">
      <path d="M0,0 L10,5 L0,10 z" fill="#37474f"/>
    </marker>
  </defs>
  <rect x="20" y="40" width="380" height="300" rx="12" fill="#f5f5f5" stroke="#bdbdbd"/>
  <text x="210" y="65" text-anchor="middle" font-size="14" font-weight="bold" fill="#37474f">活跑（在线）</text>
  <rect x="50" y="85" width="140" height="36" rx="8" fill="#e8f5e9" stroke="#2e7d32"/>
  <text x="120" y="107" text-anchor="middle" font-size="11">source</text>
  <rect x="220" y="85" width="140" height="36" rx="8" fill="#fff3e0" stroke="#ef6c00"/>
  <text x="290" y="107" text-anchor="middle" font-size="11">map → sink</text>
  <line x1="190" y1="103" x2="218" y2="103" stroke="#37474f" stroke-width="1.5" marker-end="url(#a3)"/>
  <rect x="50" y="145" width="310" height="50" rx="8" fill="#e3f2fd" stroke="#1565c0"/>
  <text x="205" y="165" text-anchor="middle" font-size="11" fill="#1565c0">start_recording(path, LZ4)</text>
  <text x="205" y="182" text-anchor="middle" font-size="9" fill="#1565c0">钩入 publish_bytes · 每条消息在飞行中捕获</text>
  <line x1="120" y1="121" x2="120" y2="143" stroke="#1565c0" stroke-width="1.2" stroke-dasharray="3 2"/>
  <line x1="290" y1="121" x2="290" y2="143" stroke="#1565c0" stroke-width="1.2" stroke-dasharray="3 2"/>
  <line x1="205" y1="195" x2="205" y2="230" stroke="#1565c0" stroke-width="2" marker-end="url(#a3)"/>
  <text x="235" y="215" font-size="10" fill="#1565c0">stop_recording()</text>
  <rect x="60" y="230" width="290" height="80" rx="8" fill="#fff8e1" stroke="#f9a825" stroke-width="1.5"/>
  <text x="205" y="252" text-anchor="middle" font-size="12" font-weight="bold" fill="#f57f17">.trec 文件（v2）</text>
  <text x="205" y="270" text-anchor="middle" font-size="9" fill="#5f6368">header + dict + chunks(LZ4) + index + stats + footer</text>
  <text x="205" y="286" text-anchor="middle" font-size="9" fill="#5f6368">血缘二进制入库 · schema 内嵌 · CRC 校验</text>
  <rect x="440" y="40" width="380" height="300" rx="12" fill="#f5f5f5" stroke="#bdbdbd"/>
  <text x="630" y="65" text-anchor="middle" font-size="14" font-weight="bold" fill="#37474f">回放（离线）</text>
  <rect x="480" y="85" width="140" height="36" rx="8" fill="#fff8e1" stroke="#f9a825"/>
  <text x="550" y="107" text-anchor="middle" font-size="11">.trec 文件</text>
  <rect x="660" y="85" width="140" height="36" rx="8" fill="#e3f2fd" stroke="#1565c0"/>
  <text x="730" y="107" text-anchor="middle" font-size="11">RecordReader</text>
  <line x1="620" y1="103" x2="658" y2="103" stroke="#37474f" stroke-width="1.5" marker-end="url(#a3)"/>
  <rect x="480" y="150" width="310" height="70" rx="10" fill="#e8f5e9" stroke="#2e7d32"/>
  <text x="635" y="175" text-anchor="middle" font-size="12" fill="#2e7d32">全新 FlowRuntime + 同图 wire</text>
  <text x="635" y="195" text-anchor="middle" font-size="10" fill="#5f6368">重发布源通道消息 → 级联重建（血缘不从文件读）</text>
  <line x1="730" y1="121" x2="730" y2="148" stroke="#37474f" stroke-width="1.5" marker-end="url(#a3)"/>
  <line x1="630" y1="121" x2="630" y2="148" stroke="#37474f" stroke-width="1.5" marker-end="url(#a3)"/>
  <rect x="480" y="245" width="310" height="60" rx="8" fill="#e8f5e9" stroke="#2e7d32"/>
  <text x="635" y="268" text-anchor="middle" font-size="14" font-weight="bold" fill="#2e7d32">39/39 bit-identical</text>
  <text x="635" y="288" text-anchor="middle" font-size="11" fill="#2e7d32">离线 == 在线</text>
  <line x1="350" y1="270" x2="478" y2="103" stroke="#f9a825" stroke-width="1.5" stroke-dasharray="6 3" marker-end="url(#a3)"/>
  <rect x="380" y="160" width="100" height="30" rx="6" fill="#fce4ec" stroke="#c62828"/>
  <text x="430" y="179" text-anchor="middle" font-size="10" fill="#c62828">ti-info</text>
</svg>

### 关键设计决策

| 决策 | 理由 |
|---|---|
| **血缘二进制入库** | LineageRecord 引用通道字典 compact ID；离线分析不跑图即可读血缘 |
| **回放血缘由级联重建** | 活跑和回放血缘语义完全一致（不从文件读，避免格式变更风险） |
| **只回放源通道消息** | 中间通道经活图级联自然重算——"离线=在线"的本质 |
| **chunk 级压缩 + CRC** | LZ4 默认（3+GB/s 解压）；chunk CRC 定位损坏；文件 CRC 整体校验 |
| **schema 内嵌字典** | 离线工具（ti-info/ti-monitor）打开文件即知每通道消息布局 |
| **分片自包含** | split/merge 是原生文件操作（每文件有完整 header/dict/index/footer） |

### demo（`record_replay_demo`）实测

```
[live]   39 outputs, record saved=yes
[file]   78 messages loaded (v2, 2 channels)
[replay] 39 inputs re-published, 39 outputs
[result] 39/39 outputs bit-identical    ← 离线 == 在线
[lineage] live/tick#38 -> live/~0#38   ← 血缘由级联重建
```

### ti-info CLI 实测

```bash
$ ti info /tmp/tianshu_record_demo_v2.trec
file: /tmp/tianshu_record_demo_v2.trec
  version:  v2
  channels: 2
  messages: 78
  duration: 0.190s

CHANNEL                           COUNT        BYTES     TYPE   RATE(hz)
live/tick                            39          624               205.5
live/~0                              39          624               205.6
```

## 运行方式

```bash
./build/<preset>/bin/state_recovery_demo   # M1：状态恢复 EXACT MATCH
./build/<preset>/bin/lidar_imu_demo        # M2：切片输入 + 区间血缘
./build/<preset>/bin/record_replay_demo    # ①：离线 == 在线（v2 格式）
./build/<preset>/bin/full_chain_demo       # 全链闭环（含 ② 组件血缘环展开）
./build/<preset>/bin/ti-info <file.trec>   # record 文件检查工具
```

## 测试与质量

| 里程碑 | 新增测试 |
|---|---|
| M0 | 区间渲染/合并、历史环有界性/字节往返/absent |
| M1 | 状态版本数 + 精确血缘串、describe 渲染、恢复确定性 |
| M2 | 切片物化 + 区间血缘串、空切片省略分支 |
| ② | 组件输出派生血缘 + 环展开 + 有界增长 |
| ① v2 | CRC 标准值、血缘 serde、读写往返、LZ4 压缩、split、merge、空文件 |
| 全局 | 262/262 CMake（GCC+Clang）· 23/23 Bazel · tidy 零警告 |
