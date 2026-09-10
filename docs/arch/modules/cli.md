# ti CLI 家族与装载器（`tianshu/cli` + `core::Launcher`）

> 状态：✅ 已实现——`ti` / `ti-launch` / `ti-monitor` / `ti-info` 四个二进制 + core 侧 `DagConfig`/`Launcher` 装载器（`ti-ctl`/`ti-console`/`ti compile` 属规划，见 §7）
> 代码：`tianshu/cli/{ti_main, ti_launch_main, ti_monitor_main, ti_info_main}.cc` · `tianshu/include/tianshu/core/launcher.h` + `tianshu/src/launcher.cc`
> 关键 ADR：[ADR-0002 与 Cyber RT 的关系](../../adr/0002-cyber-relation.md)（术语节：`ti`+`ti-*` 命名与被否名） · [ADR-0014 Console](../../adr/0014-console.md)（ti-console Phase 3 规划） · [ADR-0020 消息反射与 Monitor](../../adr/0020-message-reflection-monitor.md)（ti-monitor 解析链） · [ADR-0029 SLA](../../adr/0029-sla-compilation.md)（ti-info `--calibrate` H3 回路）
> 测试：`tests/core/launcher_test.cc`（16 用例：DagConfig 解析 / Launcher 生命周期 / `ti` 派发冒烟） · 示例：`examples/hello_dag.cc` + `examples/hello.flow`
> 最后同步：2026-09-10 · commit `c8ed440`

## 1. 职责与边界（做什么 / 明确不做什么）

**做什么**：

- **`ti`（统一入口）**：kubectl / docker 插件模式的 dispatcher——`ti <verb> [args...]` 经 `execvp` 执行 PATH 上的 `ti-<verb>` 二进制；`ti` 只派发**永不重实现**工具，新 `ti-*` 二进制零成本进统一入口。verb 合法性校验（拒 `/` `.` 前缀 `-`）；无参时探测可用动词打印 usage。
- **`ti-launch`（DAG 装载器）**：`ti-launch <flow.dag> [--mode intra|shm]`——解析 INI 子集 `DagConfig`，经 `core::Launcher` 装配组件图（工厂创建 → `init()` → `launch()`），`Ctrl-C`/SIGTERM 反序停机。等价 cyber `mainboard -d xxx.dag`（ADR-0002 兼容表）。
- **`ti-monitor`（通道监视 TUI）**：`ti-monitor <channel>... [--depth N] [--once] [--decode TYPE]`——cyber_monitor 等价物：通道列表（Hz/size）+ 详情面板（schema 解码字段或 hex dump）；vi 风键位（`j/k` 选通道、`SPACE/p` 暂停回看、`h/l` 帧步进、`CTRL-D/U` 跳 16 帧、`g/G` 首末帧、数字前缀如 `5j`）；`--once` 无头模式供 CI（每通道等首帧 3s，打印一行摘要，0 ok / 1 timeout）。
- **`ti-info`（record 检查器）**：`ti-info <file.trec> [--messages N] [--lineage] [--calibrate]`——不链接发布者代码即可读 Tianshu Record v2：摘要（版本/通道/消息数/字节/时长）、逐通道统计（count/bytes/type/rate）、消息预览（含血缘 describe）；`--calibrate` 执行 ADR-0029 H3 校准回路（§3.3）。
- **`core::Launcher`（库形态装载器）**：`DagConfig::parse/parse_file`（INI 子集，TOML 形状）+ `Launcher::start/run_until_signal/stop`——CLI 与示例（`hello_dag`）共用同一装配语义。

**明确不做什么**：

- **`ti` 不内置工具逻辑**：无 PATH 上的 `ti-<verb>` 即报错退出 127；派发不重新实现任何工具。
- **`ti-launch` 不装载 DSL flow / 编译产物**：它消费的是 core 组件 DAG（`[component]` 节 + `ComponentFactory`），不是 `IrGraph`/`.so`——ADR-0030 M-D 的"`ti launch` 按名 dry-run trace 后编译运行"尚未接线（当前该闭环的示例形态是 `examples/traceable_flow_demo.cc` 进程内自演）。
- **`DagConfig` 不是完整 TOML**：Phase 1 刻意子集（~100 行零依赖解析器），`[component <name>]` 节 + `type` / `inputs`（逗号分隔）/ `interval_ms` 三键，未知键/未知节/节外键一律报错；完整 TOML 待 [ADR-0016](../../adr/0016-config-format.md) 解析器归队。
- **`ti-monitor` 无跨机/发现能力**：通道靠显式参数指定（不扫总线），解码靠 `DecoderRegistry` + SHM sidecar（ADR-0020），查不到 schema 降级 hex dump——工具永远可用。
- **`ti-ctl`（ADR-0012 参数系统）/ `ti-console`（ADR-0014，Phase 3）/ `ti compile`（ADR-0030）未实现**。

## 2. 公共 API 速览

### 2.1 CLI 面（`tianshu/cli/`，四个独立 main）

| 二进制 | 用法 | 退出码约定 |
|---|---|---|
| `ti` | `ti <verb> [args...]`；无参 → usage + 已发现动词（rc 2）；verb 非法 → rc 2；`ti-<verb>` 不在 PATH → rc 127 | 派发成功后由子进程决定 |
| `ti-launch` | `ti-launch <flow.dag> [--mode intra\|shm]`；解析失败/装配失败 → rc 1，usage → rc 2 | 正常停机 rc 0 |
| `ti-monitor` | `ti-monitor <channel>... [--depth N] [--once] [--decode TYPE]`（depth 默认 512）；未知选项 rc 2，attach 失败 rc 1 | `--once`：全通道有帧 0 / 超时 1 |
| `ti-info` | `ti-info <file.trec> [--messages N] [--lineage] [--calibrate]`；打不开/非 v2 → rc 1 | 正常 rc 0 |

### 2.2 库面（`core/launcher.h`，namespace `tianshu::core`）

| 类型 / 函数 | 说明 |
|---|---|
| `DagComponentConfig` | `name` · `type`（工厂注册名）· `input_channels`（`vector<string>`）· `interval`（`chrono::milliseconds`，0 = 事件驱动） |
| `DagParseResult` | `components` · `error` · `ok()` |
| `DagConfig::parse(text)` / `parse_file(path)` | 静态：INI 子集解析；`#`/`;` 注释；错误即整体失败（无部分结果） |
| `Launcher(mode)` | 构造传 `transport::TransportMode`（默认 `kIntra`），持一个 `Node` |
| `Launcher::start(dag, *error)` | 逐组件 `ComponentFactory::create` → `init()` → `launch(node, inputs, interval)`；中途失败**回滚**已启动组件后返回 false |
| `Launcher::run_until_signal()` | 阻塞至 SIGINT/SIGTERM 再 `stop()`（定向等待实现，见 §3.1） |
| `Launcher::stop()` | 反序 `quiesce()` 全部自驱线程 → 反序 `shutdown()` → 清空 |
| `Launcher::components()` | `vector<unique_ptr<ComponentBase>>` 只读访问（测试/示例 introspection 用） |

`ti-monitor` 的交互面来自 `core/monitor.h` 的 `MonitorApp`（`add_channel` / `snapshot` / `pause` / `resume` / `select_delta` / `step_frame` / `jump_frame_by/first/last` / `wait_first_frames`）；`ti-info` 的数据面来自 `dsl/record_v2.h` 的 `RecordReader`（`open` / `next` / `channels` / `find_channel` / `stats` / `major_version`）——两者属 core / dsl 模块，CLI 只做渲染与编排。

### 2.3 真实用法（摘自示例 / 测试）

```text
# 来源：examples/hello.flow —— ti-launch 消费的 DAG 文件（与 hello_dag.cc 同格式）
[component source]
type = hello_source
interval_ms = 100

[component doubler]
type = hello_doubler
inputs = /demo/count
```

```cpp
// 来源：examples/hello_dag.cc —— Launcher 库形态（ti-launch 的同语义进程内版）
const auto dag = tianshu::core::DagConfig::parse_file("examples/hello.flow");
tianshu::core::Launcher launcher;                 // INTRA 模式
std::string error;
if (!launcher.start(dag, &error)) { /* ... */ }   // 工厂创建 + init + launch
std::this_thread::sleep_for(std::chrono::milliseconds(1500));
launcher.stop();                                  // 反序 quiesce → shutdown

// 来源：tests/core/launcher_test.cc（TiDispatchTest）—— ctest 经 TI_BIN_DIR 冒烟：
// execl(ti, "ti", "launch", "/nonexistent.flow") 必须由 ti-launch 接管
// （exit 1 = ti-launch 报缺文件；127 则说明 ti 派发本身失败）
```

```text
# 来源：ti_monitor_main.cc 头注释 —— 键位契约
#   j / k 选择通道 · SPACE 或 p 暂停/恢复 · h / l 前后帧（暂停时）
#   CTRL-D / CTRL-U 跳 16 帧 · g / G 首末帧 · q 退出；支持计数前缀（5j）
```

## 3. 内部设计

### 3.1 `ti` 派发与 `ti-launch` 生命周期

```mermaid
flowchart TD
    U["ti &lt;verb&gt; args..."] --> V{"verb 合法？（无 / . 前缀 -，非空）"}
    V -- "否" --> E2["rc 2"]
    V -- "是" --> X["execvp(ti-&lt;verb&gt;, args)"]
    X -- "ENOENT" --> E127["rc 127（looked for ti-&lt;verb&gt; on PATH）"]
    X -- "exec 成功" --> TL["ti-launch 进程"]
    TL --> P["DagConfig::parse_file（INI 子集）"]
    P -- "失败" --> E1["rc 1 + error"]
    P -- "ok" --> S["Launcher::start：逐组件 create → init() → launch(node, inputs, interval)"]
    S -- "任一步失败" --> RB["回滚：stop() 已启动组件 → rc 1"]
    S -- "全部成功" --> R["run_until_signal()"]
    R --> SIG["SIGINT/SIGTERM → flag 置位"]
    SIG --> Q["stop()：反序 quiesce 全部自驱线程<br/>→ 反序 shutdown → 清空"]
    Q --> OK["rc 0（stopped）"]
```

- **派发器即协议**：`ti` 用 `execvp` 原地替换进程（非 fork）——无中间层、信号语义干净；工具自测可用 `PATH=<bindir>:$PATH ti <verb>` 复现。动词发现走**探测而非扫目录**（对计划内家族 `{launch, monitor, console, ctl, inspect}` 逐个 `stat + access(X_OK)`），保持廉价——注意探测表**不含 `info`**：`ti info` 派发本身可用（`execvp ti-info`），只是不出现在无参 usage 清单里（§7）。
- **`run_until_signal` 的定向等待**：同步 handler 只翻 flag，主循环 `sigtimedwait(空集, 50ms nap)` 醒来观察——**不是**裸 `pause()`（多线程进程里异步 handler 可能在别的线程跑，本线程 `pause` 会永久睡眠，SIGTERM 测试复现过）也**不是**裸 `sigwait`（block mask 是 per-thread 的，信号落在未屏蔽线程上会直接杀进程）。该取舍以注释形式存档在 `launcher.cc`。
- **停机两段式**：先反序 `quiesce()` **全部**自驱线程，再反序 `shutdown()`——in-flight 定时 publish 会同步 dispatch 进别的组件的 Writer/CacheBuffer，必须先全员停车再拆状态（`launcher.cc` 注释）。
- **`--mode` 直通 `TransportMode`**：`ti-launch` 只认 `intra|shm`（未知值 rc 2），传给 `Launcher` 构造 `Node`——跨进程部署即 `ti-launch x.flow --mode shm`。

### 3.2 `ti-monitor`：快照模型 + 终端保卫

- **渲染/逻辑分离**：`MonitorApp`（core）管缓冲与游标，TUI 每 100ms `poll(STDIN)` 醒来 `snapshot()` 重绘——渲染层无状态，`--once` 复用同一 `snapshot()` 做无头输出。
- **`TermGuard`**：进 raw 模式（`ICANON|ECHO` 关、`VMIN=VTIME=0`）+ 备用屏幕（`?1049h` / 隐藏光标），`leave()` 恢复——异常路径也不留脏终端。
- **解码优先级**：`--decode TYPE` > 通道 schema sidecar（`ChannelView.schema_type_name`，ADR-0020 Phase 2：`MonitorApp::add_channel` attach 时自动装载 `/tianshu_schema_<fnv1a>`）> hex dump；解码走 `DecoderRegistry::decode` 填 `FieldTreeView`，逐行 `name = value`。
- **计数前缀**：数字积累进 `pending_count`（上限 999），随下一个动作键消费（`5j` 下移 5 个通道；`g/G` 清零）。

### 3.3 `ti-info` 与 H3 校准回路（ADR-0029 Phase 1）

- **索引**：全文件单遍扫描建 `(channel_id << 48 | seq) → ts_ns` 索引；对每条带血缘跳的消息，**产出该消息的级的输入** = 倒数第二跳（单跳血缘则回退到 root）。
- **级时延**：`ts(out) − ts(命中的输入消息 ts)`，按输出通道聚合，排序取 P50 / P99 / P99.9。
- **WCET 建议** = `p99.9 × 1.3`（ADR-0029 H3 目标窗上界），表格输出 `STAGE(out<-in) / N / p50 / p99 / p99.9 / WCET_suggest`；尾注提醒：声明值与建议偏差 >3× 即漂移（ADR-0029 D2）——反填 `with_wcet()` 由用户执行，v0 不自动改写。

## 4. 与其它模块的关系

- **core（主要底座）**：`Launcher` 用 `ComponentFactory` + `TIANSHU_REGISTER_COMPONENT`（`core/component.h`）与 `Node`（`core/node.h`）；`ti-monitor` 用 `MonitorApp` / `DecoderRegistry` / `FieldTreeView`（`core/monitor.h` / `core/field_table.h`）。
- **transport**：`ti-launch --mode` 直通 `TransportMode`；`ti-monitor` attach SHM 通道时经 sidecar 拿 schema（[transport.md](./transport.md) §2.6）。
- **dsl**：`ti-info` 消费 record v2（`dsl/record_v2.h`，ADR-0028）；血缘 hop 的时间戳是校准回路的原始数据。
- **sla**：`--calibrate` 是 ADR-0029 H3（WCET 校准回路）的落地入口，产出反填 `with_wcet` 声明。
- **compiler**：`ti compile` 是 ADR-0030 M-B 规划的家族成员（离线编译 / `--emit-source` 审计），未实现；M-D 后 `ti launch` 将接 dry-run trace + 产物装载。
- **依赖方向**：`ti` 零库依赖（纯 POSIX）；其余三 binary 链接 `libtianshu`。CLI 目录不含库代码，全部逻辑在 core / dsl，CLI 只做参数解析与 I/O 编排——这也是 `ti` 派发器能保持 ~40 行的原因。

## 5. 设计决策与被否方案（ADR 摘要）

| # | 决策 | 被否方案与理由 | 出处 |
|---|---|---|---|
| 1 | **`ti` + `ti-*`：统一入口 + 独立工具（kubectl/docker 插件模式）** | 单体多命令二进制（工具不能独立脚本化）；每工具独立前缀（无统一发现面） | [ADR-0002 术语节](../../adr/0002-cyber-relation.md) |
| 2 | 装载器命名 **`ti-launch`** | **`mainboard`**：cyber 历史包袱、硬件隐喻错位；**`ts`**：与 moreutils 时间戳工具冲突，且实时系统语境 `ts` 首先读作 timestamp；**`tsctl` / `tictl`**：`ctl` 后缀与子命令动词结构冲突 | 同上（被否名与理由逐条记录） |
| 3 | `mainboard -d xxx.dag` ↔ `ti launch xxx.dag` 等价映射 | ——（API 兼容承诺的一部分，ADR-0002 兼容表） | ADR-0002 |
| 4 | **DagConfig = INI 子集、TOML 形状** | 直接引 TOML 库：Phase 1 依赖治理下的刻意取舍；`[section] + key = value` 与 TOML 直映射，[ADR-0016](../../adr/0016-config-format.md) 解析器到位后平移 | `launcher.h` 头注释 |
| 5 | **monitor 解析 = schema 随通道分发 + DecoderRegistry** | 工具侧预配置映射（每 channel 改配置、跨进程无法共享，ROS2 echo 教训）；仅 hex dump（不可用） | [ADR-0020](../../adr/0020-message-reflection-monitor.md) |
| 6 | **`ti-monitor --once` 无头模式** | 仅 TUI：CI 无法断言"通道活着"；`--once` 提供可脚本化契约（rc 0/1） | `ti_monitor_main.cc` |
| 7 | **console 归 `ti-console`、Phase 3 交付（TUI ftxui + Web React）** | Phase 1 提前做面板：46 点工作量全在 Phase 3；monitor 先行覆盖通道观测刚需 | [ADR-0014](../../adr/0014-console.md) |

## 6. 测试 / 基准 / 示例入口

- `tests/core/launcher_test.cc`（16 用例）：
  - **DagConfigTest × 9**：全量解析（注释/多输入）· 拒未知键 / 缺 type / 节外键 / 坏 interval / 未知节 / 无名节 · 文件缺失 · `parse_file` 往返。
  - **LauncherTest × 6**：source→echo 全链 e2e（计数断言）· 未知类型净错误 · 缺输入 launch 失败 · 中途失败回滚后新 Launcher 可用 · `run_until_signal` SIGTERM 5s 看门狗下终止不挂 · `init()` 失败净错误并停机。
  - **TiDispatchTest × 1**：`TI_BIN_DIR`（ctest 注入）下 fork + `execl(ti, "ti", "launch", ...)`，断言 exit 1（ti-launch 接管并报缺文件）而非 127（派发失败）。
- 手工验证记录（CHANGELOG）：`shm_talker` + `ti-monitor --once` 两进程免 `--decode` 跨进程自动解码 ImuData 字段（ADR-0020 Phase 2 验收）；Hello DAG 里程碑 15/15 消息全栈贯通。
- 示例：`examples/hello_dag.cc` + `examples/hello.flow`（Launcher 库形态 + ti-launch 同格式文件）；`examples/record_replay_demo.cc` 的 `.trec` 可直接喂 `ti-info`。
- 基准：无专属基准（CLI 层无热路径；相关性能属 transport / dsl 模块）。

## 7. 已知限制与演进方向

- **动词发现表不含 `info`**：`ti_main.cc` 探测 `{launch, monitor, console, ctl, inspect}`——`ti info` 派发可用但不出现在无参 usage；补一行探测或改为注册制（工具自带 `ti-verb` 清单）待议。
- **`ti-launch` 未接 DSL/编译产物**：当前只装 core 组件 DAG；ADR-0030 M-D 收尾后 `ti launch <flow名>` 应走 dry-run trace → `Pipeline::compile` → 产物装载（`traceable_flow_demo.cc` 已在进程内演示全闭环，差 CLI 接线）。随之 `IrGraph::export_dag()` 的 TOML 产物与 `DagConfig` 的合并/迁移需一并裁决。
- **家族成员缺位**：`ti compile`（ADR-0030，离线预编译——车端冷启动时延的主要缓解）、`ti-ctl`（ADR-0012 参数系统）、`ti-console`（ADR-0014，Phase 3，TUI+Web）均未实现。
- **DagConfig 子集**：无嵌套/数组/插值；`interval_ms` 仅非负整数；组件级参数（对应 cyber `.conf`）待 ADR-0012/0016 体系。
- **`ti-monitor` 通道需手输**：无发现/订阅列表（依赖 [ADR-0015](../../adr/0015-discovery-abstraction.md) DiscoveryBackend）；跨机通道不可见（Zenoh 属 Phase 2）。
- **`ti-info --calibrate` 单遍索引的内存**：`(channel, seq) → ts` 索引常驻内存，超长 record 需分片处理；建议值反填不自动（v0 刻意，避免静默改声明）。
- **`Launcher` 无健康监控**：组件 crash 后无自动重启/降级叙事（状态恢复属 DSL 流的 ADR-0027 体系，两套装载路径的融合是 Phase 2 议题）。
