# ADR-0024：DSL box 原语 —— 带端口与生命周期的节点

- **状态**：已接受（v0 范围见「分期」）
- **日期**：2026-08-31
- **决策者**：Pride Leong
- **关联**：[adr/0021](./0021-dsl-v0.md) · [adr/0022](./0022-lineage-v0.md) · [adr/0001](./0001-dsl-form.md) · [adr/0002](./0002-cyber-relation.md)

---

## 背景

全链路 demo（`examples/full_chain_demo.cc`）暴露了 v0.5 声明原语的两个结构性缺口：

1. **驱动不可复用**：`source<T>(ch, interval, lambda)` 把雷达/GNSS 的产生逻辑内联在应用里。真实驱动是跨应用共享的单元（同一雷达驱动接不同应用），声明层没有"打包一个驱动、到处引用"的表达。
2. **读写一体单元缺失**：底盘在 cyber 体系里是一个 Component（`Reader<ControlCmd>` 进 + `Writer<ChassisState>` 出 + 自启动）。demo 里被迫拆成两处——种子心跳源（自举反馈环）+ `map_to` plant（状态积分写回）。拆分有**真实缺陷**：种子源在稳态期持续注入 v=0 幽灵状态，AllLatest 会拿它与新预测误配对；且一个逻辑单元散落两个图节点，状态归属不清。

两个缺口指向同一个缺失原语：**box——带端口集合与自身生命周期的节点**。

## 候选方案

### 方案 1：维持现状（seed 源 + map_to 组合）

**否决**：幽灵状态注入是正确性缺陷不是美观问题；"演示可以、模式不可复制"；驱动复用完全没有表达。

### 方案 2：DSL 节点直接引用 L4 ComponentFactory（单一 from 原语）

`from<T>(ch, "radar_driver_v2")` 按注册名直接查组件工厂、驱动其生命周期。

**否决（现在）**：组件生命周期/调度器与 DSL 解释器的互操作（谁拥有线程、DAG 装配与 Flow 声明如何合流）值得单独一轮设计。Flow 声明层应先稳定自己的节点模型。作为演进项保留（见「复用路径」）。

### 方案 3：DSL 层 box 原语（**已选**）

在声明层引入 box：若干输入端口 + 输出端口 + 自身生命周期（`on_init` 可发布初始输出）。解释器直接执行；L1 编译器后续把 box 映射到组件装配——**box 就是 DSL 声明层的 Component 投影**。

## 决策

**选方案 3**，v0 形态锁定如下。

### 1. API 面（v0：一进一出 + 生命周期）

```cpp
struct ChassisMain {
  void on_init(tianshu::dsl::BoxPub<ChassisState>& pub);                  // 自举：可发布初始状态
  void handle(const ControlCmd& in, tianshu::dsl::BoxPub<ChassisState>& pub);  // 输入处理
};

auto chassis = builder.box<ControlCmd, ChassisState>(control_chain, "chassis", ChassisMain{...});
// 返回 FlowChain<ChassisState>：下游可 join / sink / map，与其它原语无缝组合

// 反馈环断环用：纯句柄声明（不声明生产者），join 可先引用、box 后构造
auto chassis_port = builder.tap<ChassisState>("chassis");
```

- `BoxPub<T>` 是运行期发布句柄，绑定 box 的输出通道；**只能在 on_init/handle 调用期间使用**（存下来事后调用是契约违背，v0 不做运行期检测，演进项加 assert）
- box 对象按值捕获进 wire 闭包；跨调用状态用 `shared_ptr` 成员（demo 的 plant 车速即此模式）
- `on_init` 在**装配期**（run_for 驱动 source 之前）执行

### 2. 血缘语义（与既有原语严格一致）

| 发布时机 | 输出血缘 | 等价于 |
|---|---|---|
| `handle` 内发布 | 输入消息血缘 + hop(输出通道, seq) | `map` |
| `on_init` 发布 | rooted(输出通道, seq) | `source` |

依据：出处应反映真实产生者。底盘上电报文的真实源头就是底盘盒本身（根）；处理命令产生的状态则携带整条加工链。实现：`BoxPub` 携带可选父上下文，运行时在 handle 调用前置入、调用后清除。

### 3. 自举语义（替代 seed hack）

`on_init` 在装配期发布初始输出 → 反馈环的 join 在首个 source tick 前就有第一条底盘消息 → **环自然点火**。对比旧方案：无幽灵状态（init 只发一次，非周期心跳）、无速率耦合、一个逻辑单元一个图节点。

### 4. 复用路径（分期）

- **现在（源码级）**：驱动/底盘打包为库类型（如 `ChassisMain`），任何应用构造同类型交给 `builder.box`——C++ 组合即复用
- **演进（部署级）**：**单一引用原语** `from<T...>(name, [输入链])` 按注册名绑定 L4 ComponentFactory（`TIANSHU_REGISTER_COMPONENT` 已在）；被引用组件的端口形状由其注册签名决定，DSL 侧只声明期望类型做编译期校验（0 进口组件即无输入链调用）。**不按节点种类造词**——节点语义（source/map/join/block）与实现来源（inline/注册）是正交维度，引用机制只作用于后一轴，一个动词足够（初稿曾并列 `source_from`/`box_from` 两词，属词汇按 M×N 膨胀的设计错误，已废弃）。届时同一驱动 .so 服务 DSL flow 与 DAG 两种前端
- **契约面（已具备）**：`MessageTraits`（类型/序列化）+ schema sidecar（ADR-0020，工具免配置观测）

### 5. 执行模型约束（承袭 ADR-0021/0022）

- `handle` 在 dispatch 线程内联执行（同步级联），与 map/join 同约束；box 状态无需锁
- 输出通道的发布走每消费者邮箱扇出——box 输出天然多消费者

## 影响范围

- `flow.h`：`Flow::BoxDecl` + `FlowBuilder::box`（`describe()` 渲染 `box[in -> out]`）
- `dsl_runtime.h`：`BoxPub<T>` + `FlowRuntime::attach_box`
- `full_chain_demo.cc`：底盘改写为 `ChassisMain` 盒子，删种子源与 plant `map_to`
- 测试：box 生命周期（init 根血缘 / handle 派生血缘）+ 无 seed 闭环自举
- ADR-0021 增补一行指向本 ADR；功能点进开发计划（L1-DSL v0.6 子集）

## 风险与缓解

| 风险 | 缓解 |
|---|---|
| `BoxPub` 被存储事后调用（父上下文悬垂） | 契约文档化（v0）；演进：调用窗口 assert |
| box 状态生命周期不清晰 | 按值捕获 + `shared_ptr` 状态的既定模式写入 API 注释与 demo 示例 |
| 多输入/纯输出盒缺失（驱动盒、融合盒） | v0 用 join/map 组合可覆盖；0 进口/多端口盒为演进项，Source 可重表达为 0 进口盒（统一机会，不破坏现有 API） |
| box 与 L4 Component 概念混淆 | 本 ADR 明确：box 是声明层节点，Component 是装配层单元；合流点在 L1 编译器，合流前两者并存各司其职 |
