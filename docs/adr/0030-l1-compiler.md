# ADR-0030：L1 编译器 —— 六阶段管线与零开销 codegen

- **状态**：已接受
- **日期**：2026-09-03
- **决策者**：Pride Leong
- **关联**：[adr/0001](./0001-dsl-form.md) · [adr/0021](./0021-dsl-v0.md) · [adr/0022](./0022-lineage-v0.md) · [adr/0029](./0029-sla-compilation.md) · [01-roadmap §1.2-1.4](../01-roadmap.md)

---

## 需求清单（评审输入，逐条锁定）

| # | 需求 | 来源 |
|---|---|---|
| 1 | 六阶段加载期编译：trace → 分析 → 优化 → SLA 规划 → codegen → 装载，产出 `.so` + `.dag` + `.conf` | README 目标形态 · ADR-0001 |
| 2 | **零运行时开销**：codegen 产物与手写代码 P99 差异 <1%（H2 验收），这是专利核心承诺与框架存在理由 | 01-roadmap §1.3 |
| 3 | 逐字节输出等价：编译产物与手写 baseline 的消息流完全一致（H1 验收） | 01-roadmap §1.2 |
| 4 | 血缘不丢：codegen 产物必须逐消息携带与解释执行相同语义的血缘（ADR-0022/0026/0027 已验证的能力不得回退） | 0022/0026/0027 |
| 5 | 解释执行路径保留为 escape hatch（trace 失败/无编译器环境/mcu 等场景） | ADR-0001 工程稳健性 |
| 6 | SLA 分析（ADR-0029）作为 pass 3 消费者接入管线，判定结果写进 `.conf` | 0029 D7 |

## 决策

### D1：编译目标形态 —— C++ 源码生成 + 加载期编译成 `.so`

不走 LLVM IR / 内存 JIT，走**源码 codegen + 系统编译器**：

- Pass 4 从 IR 生成一份自包含 C++ 转换单元（`<flow>.gen.cc`）：每个 DSL 节点直译为
  `DataDispatcher::dispatch` 的直接回调注册，算子函数体以 **lambda 内联**（编译器可再内联），
  血缘代码与手写路径共用同一个 `Lineage` 内联实现。
- 加载期调用系统编译器（`c++ -std=c++20 -O2/ -O3 -fPIC -shared`，编译器与 flags 从
  配置发现：`CXX` 环境变量 → `c++`）产出 `.so`，`dlopen` + 固定工厂符号装载。
- 产物缓存：以 **IR 规范化哈希** 为键（`<flow>.<hash8>.so`），未变的图不重编。

理由：零开销承诺靠 `-O3` + 同一份运行时内联头达成，而非自造后端；源码可审计（安全认证
需要人读的产物）；工具链依赖与构建系统一致（无新增）。否决项见"被否决的备选"。

### D2：IR —— 类型化声明图（`tianshu/compiler/ir.h`）

现有 `Flow` 声明图（SourceDecl/MapDecl/JoinDecl/OpDecl/StatefulDecl/SpanDecl/FromDecl/SinkDecl）
**升级为编译器 IR**，不另造中间表示：

- 补齐三个编译必需字段：每节点 `wcet`（接 ADR-0029 预算表）、拓扑序（加载期排定）、
  `normalize()`（规范化：通道名去匿名化重排 + 稳定哈希，作为缓存键）。
- IR 是唯一 pass 间契约：pass 只读/改写 IR，不触碰 DSL 类型。
- 序列化格式 v0 直接复用 record v2 的 chunk 编码器（ADR-0028），`.dag`/`.conf`
  从 IR 派生：`.dag` = 拓扑 + 通道表，`.conf` = SLA 判定 + 预算表 + 编译 flags。

### D3：六阶段 pass 管线（`tianshu/compiler/pipeline.h`）

| Pass | 输入 → 输出 | 内容 | 现状 |
|---|---|---|---|
| P0 trace | flow 函数 → Flow 声明图 | 现有 FlowBuilder 即 trace 产物（dry-run 形态 M-C 再补） | ✅ 已有 |
| P1 validate+analyze | Flow → Flow | 图合法性（悬挂通道/类型一致/环检测）、拓扑排序 | 新增（薄） |
| P2 optimize | Flow → Flow | v0 只做**直通消除**（恒等 map 折叠）与常量通道传播；融合/内联交给 C++ 编译器 | 新增（薄） |
| P3 sla-plan | Flow → SlaReport+预算表 | **就是 ADR-0029 分析器**，判定写 `.conf` | ✅ 已有 |
| P4 codegen | Flow → `.gen.cc` | D1 的源码生成 | 核心 new |
| P5 emit+load | `.gen.cc` → `.so` + 装载 | 编译、缓存、dlopen、工厂绑定 | 核心 new |

v0 管线在**进程内一次执行**（`pipeline::compile(Flow) → CompiledFlow`），不落盘中间产物；
`ti compile <flow>` CLI（M-B）暴露同一管线做离线编译。

### D4：codegen 细节 —— 与手写逐符号对齐

零开销的实现手段是**让产物长得和手写一模一样**：

1. 节点直译：`map(f)` → `dispatcher.attach(in, out, [f](auto& m){ dispatch(out, f(m), hop(lin, out)); })`
   ——与 `FlowRuntime` 手写路径同一 API、同一头文件、同一内联级别。
2. join/stateful/span 各自生成其手写等价物（复用 dsl_runtime.cc 的 wiring 逻辑——
   codegen 模板直接抄运行时的语义实现，保证 H1 逐字节一致）。
3. 血缘：`rooted/add_hop/merge` 全部 `inline` 头文件（已满足），产物中获得与解释执行
   相同的内联机会；**实测基准（lineage_benchmark：单跳 51ns）进 H2 对比基线**。
4. 断言与错误路径：产物内 SLA 预算表生成 `static_assert` 级注释 + 运行时旁路直方图挂点
   （ADR-0029 v0.5），不在热路径留分支。

### D5：H2 验证装置（与 codegen 同仓交付）

`benchmarks/codegen_vs_handwritten.cc`：roadmap §1.3 的 5 类链路（短/中/长/扇入/扇出），
同仓两份实现——`FlowRuntime` 解释执行 vs `CompiledFlow` 产物 vs **纯手写 dispatcher 回调**
（三方对比，手写为金标准），1M 迭代 P50/P99/P99.9。验收门：产物 vs 手写 P99 <1%；
解释执行仅作回归参考（预期显著慢于两者，差距本身是编译器价值的数据）。

失败处理沿用 roadmap：1-5% 调 pass（融合、`__attribute__((flatten))`）；>5% 修订 codegen
策略；>20% 触发架构回炉评审。

### D6：escape hatch 与安全姿态

- `CompiledFlow::fallback_interpreter()`：P4/P5 任一失败（无编译器/编译失败/沙箱环境）
  自动回落现有解释执行，日志标注 degraded。
- 产物审计：`.gen.cc` 落缓存目录（`build/tianshu-gen/`），`ti compile --emit-source` 打印；
  认证场景要求人审产物时可直接读。
- 编译器调用走白名单 flags（`-std=c++20 -O2/-O3 -fPIC -shared -I<include>`），
  不透传用户输入到 shell。

### D7：模块与里程碑

```
tianshu/include/tianshu/compiler/{ir.h, pipeline.h, codegen.h}
tianshu/src/{ir.cc, pipeline.cc, codegen.cc}          # 新链接库 libtianshu_compiler
cli/ti_compile_main.cc                                 # M-B: ti compile
benchmarks/codegen_vs_handwritten.cc                   # M-C: H2 装置
```

| 里程碑 | 内容 | 验收 |
|---|---|---|
| M-A | IR 字段补齐 + normalize + 哈希 + `.dag`/`.conf` 导出 | describe 与导出一致 |
| M-B | P1-P5 管线 + 源码 codegen + `.so` 装载 + 缓存 + `ti compile` | 单链（source→map→sink）产物运行 = 解释执行输出 |
| M-C | 全节点种类 codegen + **H2 三方对比装置** | roadmap 五链路 P99 <1% |
| M-D | dry-run trace（ti launch 启动重放）+ `REGISTER_TRACEABLE_FLOW` | README 目标 API 首行成真 |

M-B 是最小可演示闭环，M-C 是 Phase 1 的 H2 判决日。

## 被否决的备选

| 备选 | 否决理由 |
|---|---|
| LLVM JIT（ORC/Cranelift） | 工具链体积与依赖治理冲突（ADR-0005 轻架构）；产物不可人读，认证成本高；系统编译器 `-O3` 已够零开销 |
| 运行时解释执行 + 只做 SLA 检查 | 丢掉零开销承诺 = 丢掉框架身份；H2 无从谈起 |
| 自研 DSL 语法解析（外部文本 DSL） | ADR-0001 已裁决 C++ 内嵌 fluent builder；文本 DSL 是多语言 SDK 层的事（ADR-0007） |
| codegen 绕开 DataDispatcher 直写 transport | 偏离"与手写同一 API"的等价基线，H1 逐字节一致性难证；dispatcher 抽象本就零成本（inline 转发） |
| 每个 flow 一个线程的静态调度（现在做） | 执行模型变更属 Phase 2（协程/静态调度，ADR-0019）；v0 codegen 复刻现有级联语义，一次只变一个变量 |

## 风险与开放问题

- **加载期编译时延**：首编译一条中链预计秒级（进程内 `c++` 调用）；缓存 + `ti compile`
  离线预编译覆盖。部署敏感场景（车端冷启动）需 M-D 后实测，超阈值则上 AOT-only 模式。
- **编译器环境依赖**：目标机无 `c++` 时只能跑解释执行或预编译产物——profile 语义里
  `vehicle` 以上默认 AOT，`mcu` 永远 AOT。
- **`.gen.cc` 与运行时头文件版本耦合**：缓存键需包含运行时 ABI 哈希（版本号 + 关键头
  内容哈希），升级库后旧缓存必须失效。
- H1 的"逐字节一致"在浮点场景（`-ffast-math` 类差异）需限定比较口径：产物与手写用
  **同一编译 flags**，消除非确定性来源。
