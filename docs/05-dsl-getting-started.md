# 05 · DSL 入门：最简声明式链

> 状态：已实现
> 关联：[adr/0021](./adr/0021-dsl-v0.md) · [adr/0022](./adr/0022-lineage-v0.md)
> 运行：`./build/<preset>/bin/dsl_demo`

---

## 这是什么

`dsl_demo` 是 TIANSHU 声明式 DSL 的**最简可运行示例**——一条线性链，展示三个核心概念：

1. **链式 API**：`source → map → map → sink` 一条表达式声明整张图
2. **自动血缘**：每条输出消息携带完整加工履历，零用户代码
3. **解释执行**：`FlowRuntime` 直接驱动图（L1 编译器的输入是同一份声明）

## 拓扑

<svg viewBox="0 0 880 200" xmlns="http://www.w3.org/2000/svg" font-family="sans-serif">
  <defs>
    <marker id="d1" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6" orient="auto">
      <path d="M0,0 L10,5 L0,10 z" fill="#37474f"/>
    </marker>
  </defs>
  <rect x="20" y="70" width="160" height="50" rx="10" fill="#e8f5e9" stroke="#2e7d32" stroke-width="1.5"/>
  <text x="100" y="90" text-anchor="middle" font-size="13" font-weight="bold">source</text>
  <text x="100" y="106" text-anchor="middle" font-size="10" fill="#5f6368">demo/ticks · 20ms</text>
  <rect x="240" y="70" width="160" height="50" rx="10" fill="#fff3e0" stroke="#ef6c00" stroke-width="1.5"/>
  <text x="320" y="90" text-anchor="middle" font-size="13" font-weight="bold">map ×2</text>
  <text x="320" y="106" text-anchor="middle" font-size="10" fill="#5f6368">tick → ×2 → +0.5</text>
  <rect x="460" y="70" width="160" height="50" rx="10" fill="#f3e5f5" stroke="#7b1fa2" stroke-width="1.5"/>
  <text x="540" y="90" text-anchor="middle" font-size="13" font-weight="bold">sink</text>
  <text x="540" y="106" text-anchor="middle" font-size="10" fill="#5f6368">打印值 + 血缘</text>
  <line x1="180" y1="95" x2="238" y2="95" stroke="#37474f" stroke-width="2" marker-end="url(#d1)"/>
  <line x1="400" y1="95" x2="458" y2="95" stroke="#37474f" stroke-width="2" marker-end="url(#d1)"/>
  <text x="440" y="50" text-anchor="middle" font-size="11" fill="#5f6368">血缘自动级联（ADR-0022）</text>
  <text x="440" y="65" text-anchor="middle" font-size="9" font-family="monospace" fill="#7b1fa2">ticks#0 → ~0#0 → ~1#0</text>
  <rect x="670" y="70" width="180" height="50" rx="10" fill="#eceff1" stroke="#607d8b"/>
  <text x="760" y="90" text-anchor="middle" font-size="11" fill="#607d8b">FlowRuntime</text>
  <text x="760" y="106" text-anchor="middle" font-size="9" fill="#607d8b">解释执行 · 600ms</text>
  <line x1="620" y1="95" x2="668" y2="95" stroke="#607d8b" stroke-width="1.2" stroke-dasharray="4 3" marker-end="url(#d1)"/>
</svg>

## 声明代码（完整）

```cpp
const auto flow = tianshu::dsl::FlowBuilder("demo")
    .source<TickMsg>("ticks", std::chrono::milliseconds(20),
                     [](std::uint64_t t) { return TickMsg{.tick = t}; })
    .map<DoubledMsg>([](const TickMsg& in) {
      return DoubledMsg{.tick = in.tick,
                        .value = static_cast<double>(in.tick) * 2};
    })
    .map<ScaledMsg>([](const DoubledMsg& in) {
      return ScaledMsg{.tick = in.tick, .value = in.value + 0.5};
    })
    .sink([&](const ScaledMsg& msg, const Lineage& lin) {
      // (value, lineage) 一起到达 sink——血缘零用户代码
      printf("tick=%llu value=%f  lineage: %s\n", msg.tick, msg.value, lin.describe().c_str());
    })
    .build();

tianshu::dsl::FlowRuntime runtime;
runtime.run_for(flow, std::chrono::milliseconds(600));
```

## 实测输出

```
flow: flow demo: src[demo/ticks] map[demo/ticks -> demo/~0] map[demo/~0 -> demo/~1] sink[demo/~1]
tick=0 value=0.500000  lineage: demo/ticks#0 -> demo/~0#0 -> demo/~1#0
tick=1 value=2.500000  lineage: demo/ticks#1 -> demo/~0#1 -> demo/~1#1
tick=2 value=4.500000  lineage: demo/ticks#2 -> demo/~0#2 -> demo/~1#2
...
```

### 怎么读

- **拓扑声明**（第一行）：`FlowBuilder` 把链式调用记录为声明图——`src`（源）、`map`（变换）、`sink`（终端）。中间通道自动命名 `~0`、`~1`（匿名通道 = 不构成契约，编译器可融合——ADR-0027 通道分类学）
- **血缘自动级联**：`demo/ticks#1 → demo/~0#1 → demo/~1#1` 读作"源通道第 1 条消息 → 第一级变换第 1 条输出 → 第二级变换第 1 条输出"。`sink` 的第二个参数 `Lineage&` 是唯一可见点

## 体现的特性

| 特性 | 在 demo 中的位置 |
|---|---|
| 链式声明 API | `source → map → map → sink → build()` 一条表达式 |
| 类型化边（Stream\<T\>） | `.map<DoubledMsg>` 只需指定输出类型，输入由链携带（接错线 = 编译错误） |
| 匿名通道 | `~0`、`~1` 自动生成——声明了"这段接线不构成契约"（ADR-0027） |
| 自动血缘 | `Lineage&` 在 sink 到达；用户没有写任何血缘代码 |
| 同步级联 | 一个 source tick 驱动整条链（map 的输出立即触发下一级） |
| 声明/执行分离 | `Flow`（声明图）与 `FlowRuntime`（解释器）分离——L1 编译器消费同一份声明 |

## 下一步

| 想看什么 | 去哪 |
|---|---|
| 多路融合（join）+ 反馈环 | [03 · 全链路 Demo](./03-full-chain-demo.md) |
| 状态通道 + 故障恢复 | [04 · 数据模型一般化 §M1](./04-data-model-generalization.md) |
| 切片输入（雷达×IMU） | [04 · 数据模型一般化 §M2](./04-data-model-generalization.md) |
| 录制/回放 | [04 · 数据模型一般化 §Record](./04-data-model-generalization.md) |
