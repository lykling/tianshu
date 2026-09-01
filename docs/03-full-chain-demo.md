# 03 · 全链路闭环 Demo：多传感接入 → 感知 → 预测 → 规划 → 控制 → 底盘（含双反馈）

> 状态：v3（2026-08-31）· 运行：`./build/<preset>/bin/full_chain_demo`
> 关联：[adr/0021](./adr/0021-dsl-v0.md) · [adr/0022](./adr/0022-lineage-v0.md) · [adr/0024](./adr/0024-dsl-op-primitive.md) · [adr/0025](./adr/0025-from-component-reference.md)

---

## 1. Demo 要演示什么

一条最小但**结构完整**的自动驾驶纵列控制链路：

- **多路传感接入**：2 × 雷达（20ms / 25ms）+ GNSS（100ms）
- **感知**：双雷达障碍物融合，再与 GNSS 位姿对齐到世界系
- **预测**：由障碍物数导出风险度
- **规划**：融合"预测 + 底盘反馈"给出目标速度与曲率
- **控制**：**也读底盘反馈**——闭环在实测车速/转向上（非规划内嵌副本）
- **底盘**：注册组件，`init()` 发上电报文自举反馈环，`proc` 积分指令
- **反馈**：底盘通道被**规划 join、控制 join、日志 sink 三个消费者**同时消费

全部设备（雷达 ×2 / GNSS / 底盘）都是**注册组件**（`TIANSHU_REGISTER_COMPONENT`），flow 里只有名字、通道与节奏——零实现代码。

## 2. 拓扑与声明

<svg viewBox="0 0 1080 470" xmlns="http://www.w3.org/2000/svg" font-family="sans-serif">
  <defs>
    <marker id="arr" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">
      <path d="M0,0 L10,5 L0,10 z" fill="#37474f"/>
    </marker>
    <marker id="arrR" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">
      <path d="M0,0 L10,5 L0,10 z" fill="#c62828"/>
    </marker>
  </defs>

  <!-- sensors (registered, dashed) -->
  <rect x="20" y="40" width="150" height="44" rx="8" fill="#e8f0fe" stroke="#5f6368" stroke-dasharray="6 3"/>
  <text x="95" y="58" text-anchor="middle" font-size="13">radar/front · 20ms</text>
  <text x="95" y="75" text-anchor="middle" font-size="10" fill="#5f6368">from avp.radar.front</text>

  <rect x="20" y="104" width="150" height="44" rx="8" fill="#e8f0fe" stroke="#5f6368" stroke-dasharray="6 3"/>
  <text x="95" y="122" text-anchor="middle" font-size="13">radar/rear · 25ms</text>
  <text x="95" y="139" text-anchor="middle" font-size="10" fill="#5f6368">from avp.radar.rear</text>

  <rect x="20" y="240" width="150" height="44" rx="8" fill="#e8f0fe" stroke="#5f6368" stroke-dasharray="6 3"/>
  <text x="95" y="258" text-anchor="middle" font-size="13">gnss · 100ms</text>
  <text x="95" y="275" text-anchor="middle" font-size="10" fill="#5f6368">from avp.gnss</text>

  <!-- fusion join -->
  <rect x="240" y="72" width="110" height="44" rx="10" fill="#fff3e0" stroke="#ef6c00"/>
  <text x="295" y="90" text-anchor="middle" font-size="13">join 融合</text>
  <text x="295" y="106" text-anchor="middle" font-size="10" fill="#5f6368">avp/~0</text>

  <!-- perception join -->
  <rect x="420" y="72" width="110" height="44" rx="10" fill="#fff3e0" stroke="#ef6c00"/>
  <text x="475" y="90" text-anchor="middle" font-size="13">join 感知</text>
  <text x="475" y="106" text-anchor="middle" font-size="10" fill="#5f6368">avp/~1</text>

  <!-- prediction map -->
  <rect x="585" y="72" width="100" height="44" rx="10" fill="#e8f5e9" stroke="#2e7d32"/>
  <text x="635" y="90" text-anchor="middle" font-size="13">map 预测</text>
  <text x="635" y="106" text-anchor="middle" font-size="10" fill="#5f6368">avp/~2</text>

  <!-- planning join -->
  <rect x="730" y="72" width="110" height="44" rx="10" fill="#fff3e0" stroke="#ef6c00"/>
  <text x="785" y="90" text-anchor="middle" font-size="13">join 规划</text>
  <text x="785" y="106" text-anchor="middle" font-size="10" fill="#5f6368">avp/~3</text>

  <!-- control join (reads chassis too) -->
  <rect x="730" y="200" width="110" height="44" rx="10" fill="#fff3e0" stroke="#ef6c00"/>
  <text x="785" y="218" text-anchor="middle" font-size="13">join 控制</text>
  <text x="785" y="234" text-anchor="middle" font-size="10" fill="#5f6368">avp/~4</text>

  <!-- chassis component -->
  <rect x="900" y="200" width="130" height="44" rx="8" fill="#e8f0fe" stroke="#5f6368" stroke-dasharray="6 3"/>
  <text x="965" y="218" text-anchor="middle" font-size="13">from 底盘</text>
  <text x="965" y="234" text-anchor="middle" font-size="10" fill="#5f6368">avp.chassis</text>

  <!-- chassis channel bar -->
  <rect x="1052" y="30" width="14" height="340" rx="6" fill="#1565c0"/>
  <text x="1059" y="390" text-anchor="middle" font-size="11" fill="#1565c0">avp/chassis 通道</text>

  <!-- forward arrows -->
  <line x1="170" y1="62" x2="238" y2="84" stroke="#37474f" stroke-width="1.6" marker-end="url(#arr)"/>
  <line x1="170" y1="126" x2="238" y2="104" stroke="#37474f" stroke-width="1.6" marker-end="url(#arr)"/>
  <line x1="350" y1="94" x2="418" y2="94" stroke="#37474f" stroke-width="1.6" marker-end="url(#arr)"/>
  <line x1="530" y1="94" x2="583" y2="94" stroke="#37474f" stroke-width="1.6" marker-end="url(#arr)"/>
  <line x1="685" y1="94" x2="728" y2="94" stroke="#37474f" stroke-width="1.6" marker-end="url(#arr)"/>
  <!-- plan down to control -->
  <path d="M 785 116 L 785 198" fill="none" stroke="#37474f" stroke-width="1.6" marker-end="url(#arr)"/>
  <!-- gnss arc into perception -->
  <path d="M 170 262 C 300 262, 330 130, 418 100" fill="none" stroke="#37474f" stroke-width="1.6" marker-end="url(#arr)"/>
  <!-- control -> chassis component -->
  <line x1="840" y1="222" x2="898" y2="222" stroke="#37474f" stroke-width="1.6" marker-end="url(#arr)"/>
  <!-- chassis component -> channel -->
  <line x1="1030" y1="222" x2="1050" y2="222" stroke="#37474f" stroke-width="1.6" marker-end="url(#arr)"/>

  <!-- feedback (red dashed): channel -> planning, channel -> control -->
  <path d="M 1052 80 L 840 80 L 797 112" fill="none" stroke="#c62828" stroke-width="2" stroke-dasharray="7 4" marker-end="url(#arrR)"/>
  <path d="M 1052 170 L 985 170 L 985 196 L 846 218" fill="none" stroke="#c62828" stroke-width="2" stroke-dasharray="7 4" marker-end="url(#arrR)"/>
  <text x="925" y="62" text-anchor="middle" font-size="12" fill="#c62828">反馈 ① 底盘 → 规划</text>
  <text x="930" y="158" text-anchor="middle" font-size="12" fill="#c62828">反馈 ② 底盘 → 控制</text>

  <!-- sinks -->
  <rect x="420" y="150" width="110" height="26" rx="6" fill="#f3e5f5" stroke="#7b1fa2"/>
  <text x="475" y="167" text-anchor="middle" font-size="11">sink 感知采样</text>
  <line x1="475" y1="116" x2="475" y2="148" stroke="#7b1fa2" stroke-width="1.2" stroke-dasharray="3 3"/>
  <rect x="730" y="280" width="110" height="26" rx="6" fill="#f3e5f5" stroke="#7b1fa2"/>
  <text x="785" y="297" text-anchor="middle" font-size="11">sink 规划采样</text>
  <line x1="785" y1="244" x2="785" y2="278" stroke="#7b1fa2" stroke-width="1.2" stroke-dasharray="3 3"/>
  <rect x="900" y="300" width="130" height="26" rx="6" fill="#f3e5f5" stroke="#7b1fa2"/>
  <text x="965" y="317" text-anchor="middle" font-size="11">sink 底盘轨迹</text>
  <path d="M 1059 370 L 1059 330" fill="none" stroke="#7b1fa2" stroke-width="1.2" stroke-dasharray="3 3"/>

  <!-- legend -->
  <rect x="20" y="330" width="330" height="96" rx="8" fill="#fafafa" stroke="#e0e0e0"/>
  <rect x="34" y="344" width="26" height="16" fill="#e8f0fe" stroke="#5f6368" stroke-dasharray="5 2"/>
  <text x="68" y="356" font-size="11">注册组件（from 引用，虚线框）</text>
  <line x1="34" y1="378" x2="60" y2="378" stroke="#c62828" stroke-width="2" stroke-dasharray="6 3"/>
  <text x="68" y="382" font-size="11">反馈边（红虚线）</text>
  <rect x="34" y="396" width="10" height="16" fill="#1565c0"/>
  <text x="68" y="408" font-size="11">底盘通道（三消费者：规划 / 控制 / 日志）</text>
  <rect x="34" y="344" width="0" height="0"/>
</svg>

flow 声明（`examples/full_chain_demo.cc`，节选）：

```cpp
auto radar_front = builder.from<RadarMsg>("avp.radar.front", "radar/front", 20ms);
auto radar_rear  = builder.from<RadarMsg>("avp.radar.rear",  "radar/rear",  25ms);
auto gnss        = builder.from<GnssMsg>("avp.gnss",         "gnss",       100ms);

auto fused       = builder.join<RadarMsg, RadarMsg, ObstacleList>(radar_front, radar_rear, ...);
auto perception  = builder.join<ObstacleList, GnssMsg, PerceptionOut>(fused, gnss, ...);
auto prediction  = perception.map<PredictionOut>(...);

auto chassis_port = builder.tap<ChassisState>("chassis");            // 断环端口
auto plan    = builder.join<PredictionOut, ChassisState, Plan>(prediction, chassis_port, ...);
auto control = builder.join<Plan, ChassisState, ControlCmd>(         // ★ 控制也读反馈
    plan, chassis_port, [](const Plan& p, const ChassisState& ch) {
      return ControlCmd{.accel = 2.0 * (p.target_speed - ch.speed),   // 实测车速
                        .steer_cmd = p.curvature + 0.2 * (p.curvature - ch.steer)};
    });
auto chassis  = builder.from<ControlCmd, ChassisState>("avp.chassis", control, "chassis");
```

运行时打印的图声明（实测）：

```
flow avp:
  join[avp/radar/front + avp/radar/rear -> avp/~0]
  join[avp/~0 + avp/gnss -> avp/~1]
  map[avp/~1 -> avp/~2]
  join[avp/~2 + avp/chassis -> avp/~3]      ← 规划 × 底盘
  join[avp/~3 + avp/chassis -> avp/~4]      ← 控制 × 底盘（双反馈）
  from[avp.radar.front -> avp/radar/front]
  from[avp.radar.rear  -> avp/radar/rear]
  from[avp.gnss        -> avp/gnss]
  from[avp/~4 via avp.chassis -> avp/chassis]
  sink[avp/~1] sink[avp/~3] sink[avp/chassis]
```

## 3. 生效的机制（每个环节用到什么）

| 环节 | 机制 | 出处 |
|---|---|---|
| 双雷达融合 / 感知 / 规划 / 控制 | `join`（AllLatest：两输入皆非空触发，各消费一条） | ADR-0021 v0.5 |
| 底盘通道同时喂规划 + 控制 + 日志 | **多消费者**（每消费者独立血缘邮箱） | ADR-0022 v0.5 |
| 反馈环的图声明 | `tap`（纯句柄断环）+ `join` | ADR-0024 |
| 上电报文点燃反馈环 | 注册组件 `init()` 推迟至全图装配后执行 | ADR-0025 Q2 |
| 组件输出进声明图 | intra 泵回 reader → `publish_bytes`（rooted 血缘） | ADR-0025 Q3 |
| 设备复用 | `from()` 引用 ComponentFactory 注册项 + 输出通道注入 | ADR-0025 |
| 拆除不竞态 | `quiesce()`：run_for 返回前 join 全部 timer 线程 | ADR-0025 |

## 4. 结果

### 4.1 闭环收敛（双反馈下的实测）

<svg viewBox="0 0 900 330" xmlns="http://www.w3.org/2000/svg" font-family="sans-serif">
  <defs>
    <marker id="ax" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6" orient="auto">
      <path d="M0,0 L10,5 L0,10 z" fill="#37474f"/>
    </marker>
  </defs>
  <!-- target band 16..18 m/s -->
  <rect x="70" y="8" width="790" height="28" fill="#fce4ec"/>
  <line x1="70" y1="8" x2="860" y2="8" stroke="#c62828" stroke-width="1" stroke-dasharray="5 4"/>
  <line x1="70" y1="36" x2="860" y2="36" stroke="#c62828" stroke-width="1" stroke-dasharray="5 4"/>
  <text x="856" y="26" text-anchor="end" font-size="11" fill="#c62828">规划目标带 ≈ 16–18 m/s（随风险波动）</text>

  <!-- axes -->
  <line x1="70" y1="260" x2="872" y2="260" stroke="#37474f" stroke-width="1.4" marker-end="url(#ax)"/>
  <line x1="70" y1="260" x2="70" y2="14" stroke="#37474f" stroke-width="1.4" marker-end="url(#ax)"/>
  <text x="860" y="282" text-anchor="end" font-size="11">采样序（底盘消息计数 t）</text>
  <text x="30" y="140" font-size="11" transform="rotate(-90 30 140)">车速 v (m/s)</text>

  <!-- y ticks -->
  <g font-size="10" fill="#607d8b" text-anchor="end">
    <text x="62" y="264">0</text><text x="62" y="208">4</text><text x="62" y="152">8</text>
    <text x="62" y="96">12</text><text x="62" y="40">16</text>
  </g>
  <g stroke="#cfd8dc" stroke-width="0.8">
    <line x1="70" y1="204" x2="860" y2="204"/><line x1="70" y1="148" x2="860" y2="148"/>
    <line x1="70" y1="92" x2="860" y2="92"/><line x1="70" y1="36" x2="860" y2="36"/>
  </g>
  <!-- x ticks -->
  <g font-size="10" fill="#607d8b" text-anchor="middle">
    <text x="70" y="276">0</text><text x="195" y="276">10</text><text x="320" y="276">20</text>
    <text x="445" y="276">30</text><text x="570" y="276">40</text><text x="695" y="276">50</text>
    <text x="820" y="276">60</text>
  </g>

  <!-- curve: (0,0)(2,4.53)(7,7.96)(19,10.46)(31,12.28)(44,13.48)(55,14.48)(60,15.05) -->
  <polyline points="70,260 95,196.6 157.5,148.6 307.5,113.6 457.5,98.1 620,70.7 757.5,56.7 820,49.3"
            fill="none" stroke="#1565c0" stroke-width="2.4"/>
  <g fill="#1565c0">
    <circle cx="70" cy="260" r="4"/><circle cx="95" cy="196.6" r="4"/><circle cx="157.5" cy="148.6" r="4"/>
    <circle cx="307.5" cy="113.6" r="4"/><circle cx="457.5" cy="98.1" r="4"/><circle cx="620" cy="70.7" r="4"/>
    <circle cx="757.5" cy="56.7" r="4"/><circle cx="820" cy="49.3" r="4"/>
  </g>
  <text x="818" y="40" font-size="11" fill="#1565c0" text-anchor="middle">15.05</text>
  <text x="110" y="205" font-size="11" fill="#1565c0">4.53</text>
  <text x="240" y="128" font-size="11" fill="#1565c0">10.46</text>

  <text x="465" y="308" text-anchor="middle" font-size="11" fill="#5f6368">
    2 秒运行（双反馈闭环，指数逼近目标带；目标 = 18 − 风险×8，风险随障碍物数波动）
  </text>
</svg>

实测底盘轨迹（`full_chain_demo` 输出节选）：

```
chassis trace (feedback loop):
  t=0    v=  0.00 m/s    ← 底盘组件 init() 的上电报文（反馈环自举）
  t=2    v=  4.53 m/s
  t=7    v=  7.96 m/s
  t=19   v= 10.46 m/s
  t=31   v= 12.28 m/s
  t=44   v= 13.48 m/s
  t=55   v= 14.48 m/s
final: v=15.05 m/s
```

### 4.2 血缘：一条消息说清自己从哪来

**怎么读血缘符号**（三件套）：

| 符号 | 含义 | 例 |
|---|---|---|
| `通道#序号` | 某通道上发布的第 N 条消息（各通道独立从 0 计数） | `radar/front#7` = 前雷达第 8 帧 |
| `->` | 被下一算子消费并派生 | `A#1 -> B#0`：A 的第 1 条经算子加工产出 B 的第 0 条 |
| `\|` | 分支分隔（join 多路输入，每路一条来源链） | 三路 join 的输出有三条分支 |
| `~0` `~1` | 未命名的中间通道（map/join 输出，自动编号） | `avp/~0` = 融合 join 的输出通道 |

逐字拆一条感知血缘：

```
avp/radar/front#0 -> ~0#0 -> ~1#0  |  avp/radar/rear#0 -> ~0#0 -> ~1#0  |  avp/gnss#0 -> ~1#0
   前雷达第 0 帧 ──融合──┐
                        ├──→ 感知的第 0 条输出 ←── GNSS 第 0 条直接进感知
   后雷达第 0 帧 ──融合──┘
```

即：**这条感知消息由前雷达第 0 帧 + 后雷达第 0 帧融合、再与 GNSS 第 0 条对齐得到**。

<svg viewBox="0 0 980 420" xmlns="http://www.w3.org/2000/svg" font-family="sans-serif">
  <defs>
    <marker id="la" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6" orient="auto">
      <path d="M0,0 L10,5 L0,10 z" fill="#455a64"/>
    </marker>
  </defs>

  <!-- lane labels -->
  <g font-size="12" fill="#37474f">
    <text x="30" y="52">分支 ①</text><text x="30" y="132">分支 ②</text>
    <text x="30" y="212">分支 ③</text><text x="30" y="292">分支 ④</text>
  </g>

  <!-- lane 1: radar/front -->
  <ellipse cx="150" cy="48" rx="72" ry="18" fill="#e8f0fe" stroke="#5f6368"/>
  <text x="150" y="52" text-anchor="middle" font-size="11">radar/front#0</text>
  <rect x="290" y="30" width="70" height="34" rx="8" fill="#fff" stroke="#90a4ae"/>
  <text x="325" y="51" text-anchor="middle" font-size="11">~0#0 融合</text>
  <rect x="410" y="30" width="70" height="34" rx="8" fill="#fff" stroke="#90a4ae"/>
  <text x="445" y="51" text-anchor="middle" font-size="11">~1#0 感知</text>
  <line x1="222" y1="48" x2="288" y2="48" stroke="#455a64" stroke-width="1.4" marker-end="url(#la)"/>
  <line x1="360" y1="47" x2="408" y2="47" stroke="#455a64" stroke-width="1.4" marker-end="url(#la)"/>

  <!-- lane 2: radar/rear (same hops) -->
  <ellipse cx="150" cy="128" rx="72" ry="18" fill="#e8f0fe" stroke="#5f6368"/>
  <text x="150" y="132" text-anchor="middle" font-size="11">radar/rear#0</text>
  <rect x="290" y="110" width="70" height="34" rx="8" fill="#fff" stroke="#90a4ae"/>
  <text x="325" y="131" text-anchor="middle" font-size="11">~0#0 融合</text>
  <rect x="410" y="110" width="70" height="34" rx="8" fill="#fff" stroke="#90a4ae"/>
  <text x="445" y="131" text-anchor="middle" font-size="11">~1#0 感知</text>
  <line x1="222" y1="128" x2="288" y2="128" stroke="#455a64" stroke-width="1.4" marker-end="url(#la)"/>
  <line x1="360" y1="127" x2="408" y2="127" stroke="#455a64" stroke-width="1.4" marker-end="url(#la)"/>

  <!-- lane 3: gnss -->
  <ellipse cx="150" cy="208" rx="64" ry="18" fill="#e8f0fe" stroke="#5f6368"/>
  <text x="150" y="212" text-anchor="middle" font-size="11">gnss#0</text>
  <rect x="410" y="190" width="70" height="34" rx="8" fill="#fff" stroke="#90a4ae"/>
  <text x="445" y="211" text-anchor="middle" font-size="11">~1#0 感知</text>
  <line x1="214" y1="208" x2="408" y2="208" stroke="#455a64" stroke-width="1.4" marker-end="url(#la)"/>

  <!-- lane 4: chassis (rooted, loop) -->
  <ellipse cx="150" cy="288" rx="72" ry="18" fill="#e3f2fd" stroke="#1565c0"/>
  <text x="150" y="292" text-anchor="middle" font-size="11">chassis#0</text>
  <rect x="530" y="270" width="70" height="34" rx="8" fill="#e3f2fd" stroke="#1565c0"/>
  <text x="565" y="291" text-anchor="middle" font-size="11">~3#0 规划</text>
  <path d="M 222 288 C 340 288, 420 288, 528 288" fill="none" stroke="#455a64" stroke-width="1.4" marker-end="url(#la)"/>

  <!-- merge point: prediction hops then plan join -->
  <rect x="530" y="30" width="70" height="34" rx="8" fill="#fff" stroke="#90a4ae"/>
  <text x="565" y="51" text-anchor="middle" font-size="11">~2#0 预测</text>
  <line x1="480" y1="47" x2="528" y2="47" stroke="#455a64" stroke-width="1.4" marker-end="url(#la)"/>
  <line x1="480" y1="127" x2="528" y2="60" stroke="#455a64" stroke-width="1.4" marker-end="url(#la)"/>
  <line x1="480" y1="207" x2="528" y2="62" stroke="#455a64" stroke-width="1.4" marker-end="url(#la)"/>

  <rect x="700" y="130" width="130" height="64" rx="12" fill="#fff3e0" stroke="#ef6c00" stroke-width="1.6"/>
  <text x="765" y="155" text-anchor="middle" font-size="12">join 规划 ~3#0</text>
  <text x="765" y="173" text-anchor="middle" font-size="10" fill="#5f6368">四分支在此汇合</text>
  <line x1="600" y1="47" x2="730" y2="128" stroke="#455a64" stroke-width="1.4" marker-end="url(#la)"/>
  <line x1="600" y1="288" x2="755" y2="196" stroke="#1565c0" stroke-width="1.6" marker-end="url(#la)"/>

  <text x="765" y="230" text-anchor="middle" font-size="11" fill="#1565c0">底盘分支 = rooted（组件内部不透明，</text>
  <text x="765" y="246" text-anchor="middle" font-size="11" fill="#1565c0">输出即血缘根，ADR-0025 Q3）</text>

  <text x="490" y="356" text-anchor="middle" font-size="10" font-family="monospace" fill="#37474f">
    radar/front#0 -&gt; ~0#0 -&gt; ~1#0 -&gt; ~2#0 -&gt; ~3#0 | radar/rear#0 -&gt; ~0#0 -&gt; ~1#0 -&gt; ~2#0 -&gt; ~3#0
  </text>
  <text x="490" y="372" text-anchor="middle" font-size="10" font-family="monospace" fill="#37474f">
    | gnss#0 -&gt; ~1#0 -&gt; ~2#0 -&gt; ~3#0 | chassis#0 -&gt; ~3#0
  </text>
  <text x="490" y="396" text-anchor="middle" font-size="10" fill="#78909c">（规划消息 ~3#0 的 describe() 实测；分支数恒定 = root 去重环策略）</text>
</svg>

**控制消息的血缘进一步携带反馈跳**（控制 join 又合并了一次底盘）：

```
radar/front#0 -> ~0#0 -> ~1#0 -> ~2#0 -> ~3#0 -> ~4#0
| radar/rear#0 -> ~0#0 -> ~1#0 -> ~2#0 -> ~3#0 -> ~4#0
| gnss#0 -> ~1#0 -> ~2#0 -> ~3#0 -> ~4#0
| chassis#0 -> ~3#0 -> ~4#0        ← 底盘分支进入控制级
```

#### 环与血缘：边界截断的真相

反馈环意味着：最终输出理应能**追溯到最初始的第 0 帧数据**（chassis#k ← control#k-1 ← plan#k-1 ← chassis#k-2 ← … ← chassis#0 上电报文）。血缘是否体现这一点，取决于底盘的表达方式：

| | v2（底盘 = 内联 op，`2a13acb`） | v3（底盘 = 注册组件，当前） |
|---|---|---|
| 环祖先 | ✅ 完整展开：根是最早的帧，`~3 -> ~4 -> chassis -> ~3 -> …` 逐圈绕环（实测最终消息根为 `radar/front#0`，链上带 18 圈环跳） | ❌ 截断 |
| 本次新鲜输入 | ❌ 被挤掉（环去重"同 root 保更长分支"，老分支每圈变长，新帧总输） | ✅ 保留 |
| 分支长度 | 随迭代线性增长 | 恒定 4 |

v3 截断是**实现现状而非架构必然**（ADR-0025 Q3 已按评审修正）：组件执行循环 `run_proc` 是框架代码、`proc` 逐条同步驱动，父血缘配对完全可行；正确的降级边界是**攒批/跨线程发布**（真正的不可知点），而非组件边界本身。落地钩子 `transport::Message.lineage_ptr` 已预留。

完整真相需要两者并存（"由第 k 帧新鲜输入**和**第 0 帧传下的环祖先共同派生"）——线性链表装不下，属 L2 血缘**图**阶段。已排定的补齐路径：`Message.lineage_ptr` in-band（ADR-0022/0025 演进项）；由于 from() 桥两端分别持有输入邮箱与输出泵回，把组件输入血缘在泵回时作为父级接上即可让环跨组件边界重新展开。

### 4.3 采样输出（实测）

```
[P] t=0    obstacles=3  pose=(100,-12)         ← 感知：3 目标 + 世界系位姿
[L] t=0    target=18m/s  chassis_v=0m/s         ← 规划：看到底盘 0 速，要求加速
[C] t=0    accel=+36.16 m/s2  steer_cmd=+0.036  ← 控制：按实测车速算加速度
...
[L] t=15   target=17m/s  chassis_v=8m/s         ← 反馈生效：车速涨，目标随风险调整
```

## 5. 工程信息

| 项 | 值 |
|---|---|
| 代码 | `examples/full_chain_demo.cc`（flow 声明）· `examples/avp_devices.cc`（设备注册库）· `examples/avp_types.h` |
| 守护测试 | `tests/dsl/dsl_test.cc`：`OpBootstrapsFeedbackLoopWithoutSeedSource`、`ComponentClosesLoopViaInitBootstrap` 等 15 例全绿 |
| 回归基线 | 243/243 CMake（GCC+Clang）· 21/21 Bazel · tidy 零警告 |
| 稳定性 | quiesce 修复后 8 连跑零崩溃（此前存在拆除期竞态 SIGSEGV，已修） |

### 演进史（本 demo 反向驱动的框架能力）

| 版本 | 底盘表达 | 暴露的缺口 → 促成的能力 |
|---|---|---|
| v1（`63e880c`） | seed 心跳源 + `map_to` plant | 幽灵状态误配对 → 催生 op 原语 |
| v2（`2a13acb`） | `ChassisMain` op（on_init 自举） | 驱动/底盘不可复用 → 催生 from() |
| v3（`07f48a8`） | 注册组件 + from() 引用 | 拆除竞态 → quiesce 语义；双反馈（本版） |
