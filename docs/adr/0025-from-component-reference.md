# ADR-0025：`from()` 组件引用原语 —— DSL flow 与 L4 组件的进程内合流

- **状态**：已接受（v1 见「分期」）
- **日期**：2026-08-31
- **决策者**：Pride Leong
- **关联**：[adr/0024](./0024-dsl-op-primitive.md) · [adr/0002](./0002-cyber-relation.md) · [adr/0010](./0010-transport-shm-infra.md)

---

## 背景

ADR-0024 留下的最后一块：`from<T...>(name, [输入链])` —— 按注册名引用 L4 组件（`TIANSHU_REGISTER_COMPONENT` + ComponentFactory），让"写好的驱动/底盘"跨应用复用。当时推迟是因为三个互操作问题没有答案。本 ADR 回答它们。

## 关键事实（设计的地基）

**DSL 世界与组件世界已经共享同一个进程级 DataDispatcher。** DSL 的 `publish_bytes` 直接 dispatch；L4 `Component` 的输入是注册在同一 dispatcher 上的 `DataVisitor`（同 channel_id 哈希）。两个世界只差**输出方向**的对接：组件经 transport Writer 发布，DSL 消费者在 dispatcher 上等。因此桥 = 一个"输出泵回 reader"。

## 三问三答

### Q1 线程归属：解释器不夺组件的执行模型

- **输入驱动组件**（`Component<In, Out>`）：`proc` 在**发布者的 dispatch 线程**内联执行——与 DSL map/join/op 完全同模型（同步级联），组件无需感知自己被 flow 引用
- **定时组件**（`TimerSourceComponent`）：**组件自带定时线程**（L4 既有行为，`start(interval)` 自建线程），发布经泵回桥进 dispatcher
- 桥不创建额外线程；FlowRuntime 负责组件实例的存活与关闭

### Q2 生命周期对齐：launch 对齐装配期，init 推迟到全图就绪

| Flow 阶段 | 组件动作 |
|---|---|
| `from()` 装配 | factory 实例化 → `launch(node, 输入通道, interval)`（建 Writer/Visitor/输入桥，与 DAG Launcher 同路径）→ 挂输出泵回 reader |
| 全部节点装配完成（`init_hooks_` 阶段）| `init()` —— 与 op 的 `on_init` 同期执行：此时**所有下游消费者邮箱已注册**，组件在 init 里发布初始状态（底盘上电报文）不丢 |
| `run_for` 返回 / FlowRuntime 析构 | `shutdown()`，随后销毁（TimerComponent 析构自停线程） |

**Bootstrap 由此自然解决**：底盘组件在 `init()` 里 `publish` 初始状态（writer 在 launch 阶段已就绪），经泵回桥点燃反馈环——无需 seed 源、无需幽灵心跳，且组件代码不用为 flow 做任何改动。

### Q3 输出泵回：intra reader → 带根血缘的 publish_bytes

组件 `publish()` → transport Writer（INTRA，进程内零拷贝）→ **泵回 reader**（`node.create_reader(out_channel)`，callback 调 `FlowRuntime::publish_bytes(out_channel, data, size, Lineage::rooted(out_channel, seq))`）→ dispatcher + 每消费者邮箱扇出 → DSL 图的 join/sink 照常消费。

**血缘边界决策**：组件输出的血缘 = `rooted(输出通道)`——v1 的实现现状。

> **评审修正（2026-08-31）**：初稿以"组件是黑盒、运行时无法知道输入输出对应"为由将整个组件边界划为 rooted——**该理由不成立**。组件的执行循环 `run_proc` 是框架代码，`proc` 由框架逐条同步驱动，调用前设置父血缘上下文、`publish` 携带，与 `OpPub.parent_` 同一模式，同步级联下配对确定性成立（1 进 N 出 = N 条输出共享同一父，语义正确）。**正确的边界是"同步派生 vs 异步发布"，而非"内联 vs 组件"**：
>
> | 发布时机 | 父血缘 | 语义 |
> |---|---|---|
> | `proc` 调用栈内（绝大多数） | 本次输入 → 派生 | map 同构 |
> | 攒批/延迟/跨线程发布 | 不可知 → rooted | 真正的黑盒点 |
>
> 落地钩子已预留：`transport::Message.lineage_ptr`（今日恒空）。路径：组件基类 publish 携带父血缘 → IntraWriter 传播（进程内同步回调）→ 泵回 reader 用其作 `publish_bytes` 父级 → 环跨组件边界展开。v1 实现 rooted 属实现先后，非架构必然；SHM 跨进程的血缘随行序列化仍为独立演进项。

## API（v1）

```cpp
// 0 输入驱动（TimerSourceComponent<TOut> 类）：interval 由 flow 声明
auto radar = builder.from<RadarMsg>("avp.radar.front", 20ms);
// 1 输入读写组件（Component<TIn, TOut> 类）：输入通道 = chain 的通道
auto chassis = builder.from<ControlCmd, ChassisState>(control_chain, "avp.chassis");
```

- **通道归属对齐 L4 DAG 模型**：输入通道由部署侧（flow 的 chain）指定；输出通道由组件声明（`out_channel()` 是组件契约的一部分）。已知限制（与 DAG 世界相同，不单独恶化）：同一注册类的输出通道不可注入，双雷达场景需两个注册类——输出通道注入列为演进项
- **类型校验**：factory 返回类型擦除的 `ComponentBase`，桥 `dynamic_cast` 到期望形状（RTTI，测试外首次受限使用的例外，理由：跨编译边界的注册表查回必须动态校验）；失败返回 **invalid FlowChain**（`valid()` 可查，下游自然无操作）
- `Flow` 记录 `FromDecl`（自描述图），`describe()` 渲染 `from[reg -> out]`

## 已知限制（记录在案）

| 限制 | 说明 | 演进 |
|---|---|---|
| 进程内引用 | 组件与 flow 同进程（INTRA 泵回） | 跨进程（组件独立进程跑 DAG、flow 在工具机）随 discovery + SHM/zenoh |
| 组件输出通道不可注入 | L4 现状一致 | 输出通道进 launch 参数（需 L4 演进） |
| 血缘在组件边界降级为 rooted | 组件内部不透明 | `Message.lineage_ptr` 跨组件级联（L2） |
| 配置传递 | v1 组件 init 无参数配置 | `from` 第三参数配置块（对齐 DagConfig） |

## 影响范围

- `flow.h`：`FromDecl` + `FlowBuilder::from` 两重载
- `dsl_runtime.h/.cc`：`attach_referenced_source/attach_referenced_component` + `components_` 存活 + 析构 `shutdown()` + 泵回 reader
- demo：`examples/avp_devices.cc`（设备注册库：双雷达 + GNSS + 底盘，四个 `TIANSHU_REGISTER_COMPONENT`）+ `full_chain_demo` 改为 `from()` 引用
- 测试：驱动产流、组件闭环自举（init 发布）、注册缺失 → invalid、rooted 血缘
