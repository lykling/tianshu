# ADR-0012：参数系统（统一声明与访问、4 路配置优先级、引用/导入导出/热加载）

- **状态**：已接受
- **日期**：2026-08-10
- **决策者**：Pride Leong
- **关联**：[adr/0005](./0005-lightweight-multiplatform.md) · [adr/0007](./0007-api-spec-multi-language.md) · [adr/0009](./0009-doc-code-language.md) · [adr/0011](./0011-logging.md)

> **设计依据**：用户提供的 `apollo-base/docs/cyber/parameter.md` 7 个关键点。

---

## 背景

天枢需要统一参数系统，避免：

| 痛点 | 风险 |
|---|---|
| 多种参数来源（CLI / env / launch / file）无统一优先级 | 不同模块各选一种，行为不一致 |
| 类配置 vs 实例配置无规范 | 同一类多实例参数互相覆盖 |
| 路径处理不统一 | 相对路径基准不一致 |
| 无引用机制 | 配置重复、易漂移 |
| 无导入/导出 | 难复现现场配置 |
| 无热加载 | 改参数要重启 |
| 多语言 SDK 各自实现 | 参数类型/语义不一致 |

## 设计目标（7 个关键点对应你列的清单）

| 关键点 | 本 ADR 章节 |
|---|---|
| 1. 声明统一 + 访问统一 | §1 |
| 2. 配置方式 4 种 + 优先级 | §2 |
| 3. 文件路径处理 | §3 |
| 4. 类配置/实例配置 | §4 |
| 5. 配置引用 | §5 |
| 6. 导入/导出 | §6 |
| 7. 热加载 | §7 |

## 决策

### §1 声明统一 + 访问统一

**声明**：用宏 + 模板，所有参数走统一注册。

```cpp
// 单文件声明一个参数（声明即默认值）
TIANSHU_PARAM(int, scheduler_cpu_count, 4, "Number of CPU cores for scheduler");
TIANSHU_PARAM(double, sla_deadline_ms, 50.0, "End-to-end SLA deadline in milliseconds");
TIANSHU_PARAM(std::string, transport_default_backend, "HYBRID", "Default transport backend");
TIANSHU_PARAM(bool, enable_lineage, true, "Enable message lineage tracking");
TIANSHU_PARAM(std::vector<std::string>, enabled_flows, {}, "Flows to enable at startup");
```

宏展开：

```cpp
// 编译产物
namespace tianshu::params {
struct scheduler_cpu_count {
  static constexpr const char* name = "scheduler_cpu_count";
  using type = int;
  static constexpr int default_value = 4;
  static constexpr const char* description = "Number of CPU cores for scheduler";
};
}
```

**访问**：统一模板 API。

```cpp
// 读
int n = tianshu::param<int>("scheduler_cpu_count");

// 读（不存在返回 fallback，不抛异常）
int n = tianshu::param_or<int>("scheduler_cpu_count", 8);

// 写（仅运行时改，不持久化）
tianshu::set_param<int>("scheduler_cpu_count", 8);

// 检查存在
bool has = tianshu::has_param("scheduler_cpu_count");

// 列举所有参数名
auto names = tianshu::list_params();
```

**类型支持**：

| 类型 | 序列化 |
|---|---|
| `int / int64_t / uint64_t` | decimal |
| `double / float` | decimal floating |
| `bool` | `true` / `false` |
| `std::string` | quoted |
| `std::vector<T>` | `[v1, v2, ...]` |
| `std::map<string, T>` | `{k1: v1, k2: v2}` |
| 自定义 struct（用户实现 ParamTrait<T>） | 自定义 YAML/JSON 序列化 |

### §2 配置方式 4 种 + 优先级

**优先级**（高 → 低，高优先级覆盖低优先级）：

```
1. 命令行参数（CLI flags）
2. 环境变量（TIANSHU_<NAME>）
3. launch 文件注入（mainboard -d xxx.dag 时由 mainboard 注入）
4. 配置文件（YAML/JSON）

最后 fallback：代码默认值（TIANSHU_PARAM 宏声明）
```

#### 来源 1：命令行参数

```bash
tianshu-mainboard \
  --param scheduler_cpu_count=8 \
  --param sla_deadline_ms=30 \
  -d perception.dag
```

或简化形式：

```bash
tianshu-mainboard --scheduler_cpu_count=8 --sla_deadline_ms=30 -d perception.dag
```

#### 来源 2：环境变量

```bash
export TIANSHU_SCHEDULER_CPU_COUNT=8
export TIANSHU_SLA_DEADLINE_MS=30
tianshu-mainboard -d perception.dag
```

规则：参数名 uppercase + `TIANSHU_` 前缀。

#### 来源 3：launch 文件注入

```yaml
# perception.launch.yaml
mainboards:
  - name: perception
    dag: perception.dag
    params:
      scheduler_cpu_count: 8
      sla_deadline_ms: 30
      enabled_flows: [perception_flow, predict_flow]
```

`tianshu-launch perception.launch.yaml` 启动时把 params 注入每个 mainboard（通过 mainboard CLI）。

#### 来源 4：配置文件

主推 **TOML**（人类友好 + 严格语法），兼容 YAML / JSON（详见 [ADR-0016 配置格式选型](./0016-config-format.md)）。

```toml
# tianshu.toml（全局配置，自动搜索）
scheduler_cpu_count = 8
sla_deadline_ms = 30.0

[transport]
default_backend = "SHM"

[lineage]
enable = true
ring_buffer_size = 1000
```

**搜索路径**（按顺序，第一个找到的生效，**不合并**；如需合并显式 `import` 详见 §5）：

1. `--config-file <path>` 显式指定（`.toml` / `.yaml` / `.json` 任一）
2. `$CWD/tianshu.toml` → `$CWD/tianshu.yaml` → `$CWD/tianshu.json`（按格式优先级）
3. `$CWD/config/tianshu.{toml,yaml,json}`
4. `~/.config/tianshu/tianshu.{toml,yaml,json}`（用户级）
5. `/etc/tianshu/tianshu.{toml,yaml,json}`（系统级，车端常用）

详细对比与 feature flag（`TIANSHU_WITH_TOML/YAML/JSON`）详见 [ADR-0016](./0016-config-format.md)。

### §3 文件路径处理

参数值如果是文件路径，按以下规则解析：

| 输入 | 解析基准 |
|---|---|
| 绝对路径（`/etc/foo`） | 不解析，直接用 |
| 相对路径（`./foo` 或 `foo/bar`） | 基准 = 该参数**声明所在配置文件**的目录 |
| 用户路径（`~/foo`） | 展开 `~` 为 `$HOME` |
| 环境变量（`$VAR/foo`） | 展开环境变量 |

```yaml
# /opt/tianshu/configs/main.yaml
model_path: ../models/perception.onnx
# 解析为 /opt/tianshu/models/perception.onnx
```

**特例**：CLI/env 传入的相对路径基准是 `$CWD`（不是配置文件目录）。

API 暴露：

```cpp
std::string path = tianshu::param<std::string>("model_path");
std::string resolved = tianshu::resolve_path(path);  // 自动解析为绝对路径
```

### §4 类配置 / 实例配置

支持**类级**和**实例级**两层配置，用 namespace 隔离：

#### 类级配置（所有实例共享）

```cpp
class PerceptionComponent : public Component<CameraMsg> {
 public:
  // 类级参数声明
  TIANSHU_PARAM_CLASS(PerceptionComponent,
    int, max_objects, 100, "Maximum objects per frame";
    double, confidence_threshold, 0.5, "Detection confidence threshold";
  );
};
```

访问：

```cpp
int max = tianshu::param<int>("PerceptionComponent.max_objects");
```

#### 实例级配置（每个实例独立）

```cpp
class PerceptionComponent : public Component<CameraMsg> {
 public:
  void Init(const ComponentConfig& cfg) override {
    // 实例级参数（cfg 提供 namespace 前缀）
    instance_ns_ = cfg.instance_name();  // e.g., "perception_front"

    max_objects_ = tianshu::param_or<int>(
      instance_ns_ + ".max_objects",          // 先查实例级
      tianshu::param<int>("PerceptionComponent.max_objects")  // 后查类级
    );
  }
 private:
  std::string instance_ns_;
  int max_objects_;
};
```

配置文件示例：

```yaml
# 类级默认（所有实例共享）
PerceptionComponent:
  max_objects: 100
  confidence_threshold: 0.5

# 实例级覆盖（只影响该实例）
perception_front:
  max_objects: 200       # 前视摄像头检测更远，目标更多
  confidence_threshold: 0.7

perception_rear:
  max_objects: 50        # 后视摄像头 FOV 小
```

**优先级**：实例级 > 类级 > 全局默认 > 代码默认值。

### §5 配置引用

支持配置文件内引用其他参数，避免重复 + 易维护：

#### 基础引用语法（YAML）

```yaml
# 全局参数
vehicle:
  wheelbase: 2.8
  max_speed: 120

control:
  # 引用其他参数
  preview_distance: "${vehicle.max_speed} * 0.5"   # 字符串表达式（数值会求值）
  wheelbase_ref: "${vehicle.wheelbase}"             # 直接引用
```

#### 环境变量引用

```yaml
log:
  dir: "${env:TIANSHU_LOG_DIR:-/var/log/tianshu}"  # env 不存在时用默认
```

#### 文件 import（合并多个配置文件）

```yaml
# main.yaml
import:
  - common.yaml             # 相对当前文件
  - /etc/tianshu/vehicle.yaml  # 绝对路径
  - perception.yaml

# 本文件参数会覆盖 import 的同名参数
sla_deadline_ms: 30
```

**import 顺序**：先 import 的优先级低，后 import 的高，本文件最高（仍受 §2 优先级约束）。

#### 表达式求值（轻量）

```yaml
scheduler:
  cpu_count: 4
  big_core_count: "${cpu_count - 2}"      # 算术
  small_core_count: "${cpu_count - ${big_core_count}}"
```

支持：`+ - * / %`、`min(a,b) max(a,b)`、`abs(x)`、`?:` 三元、`${var:-default}`。

**禁用**：自定义函数、循环、条件分支（避免 YAML 变成编程语言）。

### §6 导入 / 导出

#### 导出当前运行时配置

```bash
# mainboard 运行中
tianshu-ctl export-params > current.yaml
```

输出：所有参数（来源、当前值、默认值、是否被覆盖）。

```yaml
# current.yaml
scheduler_cpu_count:
  value: 8
  default: 4
  source: cli          # cli / env / launch / file / code
  overridden: true

sla_deadline_ms:
  value: 30
  default: 50
  source: launch
  overridden: true

enable_lineage:
  value: true
  default: true
  source: code
  overridden: false
```

价值：

- 现场问题复现（导出 + 发回开发）
- CI 测试用例
- 配置 baseline 对比（diff 出当前 vs baseline）

#### 导入运行时配置（与导出对应）

```bash
tianshu-ctl import-params current.yaml
```

或 mainboard 启动时：

```bash
tianshu-mainboard --config-file current.yaml -d perception.dag
```

### §7 热加载

支持运行时修改参数，实时生效：

#### 触发方式

1. **文件 watch**：配置文件改动（inotify）
2. **API 调用**：`tianshu::set_param<T>("name", value)`
3. **控制台 RPC**：`tianshu-ctl set-param name=value`（详见 [evaluation/0003](../evaluation/0003-console.md)）
4. **Signal**：SIGHUP 触发重载（车端运维友好）

#### 回调机制

```cpp
// 注册回调
auto handle = tianshu::on_param_change<int>(
  "scheduler_cpu_count",
  [](const int& old_val, const int& new_val) {
    TIANSHU_INFO("scheduler")
      .field("old", old_val)
      .field("new", new_val)
      .log("param changed, reconfiguring");
    reconfigure_scheduler(new_val);
  }
);

// 注销
tianshu::off_param_change(handle);
```

#### 线程安全

- 写入：内部用 RWLock，多读单写
- 回调：在专门 thread 执行，不阻塞 hot path
- 用户回调责任：保证自身线程安全

#### 不可热加载的场景

| 场景 | 原因 |
|---|---|
| 编译期参数（如 `TIANSHU_PROFILE_*`） | 编译时已固化 |
| transport backend 类型 | reader/writer 已创建，切换需重启 |
| DAG 拓扑（增删算子） | 代码生成产物，需重新编译 |
| 协程栈大小 | 已分配 |

这些参数的 setter 返回 `Status::UNSUPPORTED_HOT_RELOAD`，提示用户重启。

## 与现有 ADR 协同

| ADR | 协同点 |
|---|---|
| [adr-0005 轻架构](./0005-lightweight-multiplatform.md) | 自研，不依赖 gflags；YAML 解析自研极简版本或允许 header-only yaml-cpp |
| [adr-0007 多语言 SDK](./0007-api-spec-multi-language.md) | C ABI 暴露参数读写；Python/Rust/Go/Node SDK 透明 |
| [adr-0009 双语+英文](./0009-doc-code-language.md) | 参数 description 字符串英文 |
| [adr-0011 日志](./0011-logging.md) | 热加载回调用 logger 输出 |
| [adr-0010 transport](./0010-transport-shm-infra.md) | transport 配置走参数系统 |

## 多语言 SDK

### C ABI

```c
// ffi/param_c.h
tianshu_status_t tianshu_param_get_str(const char* name, char* buf, size_t buf_size);
tianshu_status_t tianshu_param_set_str(const char* name, const char* value);
tianshu_status_t tianshu_param_list(/* out */ char*** names, /* out */ size_t* count);
tianshu_status_t tianshu_param_export_yaml(const char* path);
tianshu_status_t tianshu_param_import_yaml(const char* path);
```

### Python SDK 示例

```python
import tianshu

# 读
cpu = tianshu.param("scheduler_cpu_count", default=4)

# 写
tianshu.set_param("scheduler_cpu_count", 8)

# 列举
for name, val in tianshu.list_params().items():
    print(f"{name} = {val}")

# 热加载回调
@tianshu.on_param_change("scheduler_cpu_count")
def _(old, new):
    print(f"cpu count: {old} -> {new}")
```

## Profile 配置

| Profile | 默认搜索路径 | 热加载 |
|---|---|---|
| desktop | `./tianshu.yaml` 优先 | ✅ |
| server | `/etc/tianshu/` 优先 | ✅ |
| vehicle | `/opt/tianshu/config/` 优先 | ✅（生产谨慎） |
| embedded | `/etc/tianshu.yaml` 单文件 | ⚠️ 可选 |
| mcu | 编译期固定（无文件系统） | ❌ |

## 影响范围

### 新增 INFRA 框架

| 框架 | 作用 |
|---|---|
| F-INFRA-PARAM | 参数系统（声明/访问/4 路配置/引用/导入导出/热加载） |

### 工作量估算

| ID | 描述 | 优先级 | 估算 | Phase |
|---|---|---|---|---|
| INFRA-PARAM-1 | `TIANSHU_PARAM` 宏 + 类型注册（编译期） | P0 | 3 | 0-1 |
| INFRA-PARAM-2 | `tianshu::param<T>` 模板 API + RWLock | P0 | 3 | 1 |
| INFRA-PARAM-3 | CLI 解析（`--param name=value` + 短形式） | P0 | 2 | 1 |
| INFRA-PARAM-4 | 环境变量解析（`TIANSHU_<NAME>`） | P0 | 1 | 1 |
| INFRA-PARAM-5 | YAML 配置文件解析（自研极简或 header-only yaml-cpp） | P0 | 4 | 1 |
| INFRA-PARAM-6 | 文件路径解析（相对/绝对/~/展开） | P0 | 1 | 1 |
| INFRA-PARAM-7 | 类级/实例级 namespace 隔离 | P1 | 2 | 2 |
| INFRA-PARAM-8 | 配置引用（`${var}` + 表达式 + import） | P1 | 3 | 2 |
| INFRA-PARAM-9 | 导入/导出 YAML（含 source 信息） | P1 | 2 | 2 |
| INFRA-PARAM-10 | 热加载（文件 watch + API + RPC + SIGHUP） | P1 | 3 | 2 |
| INFRA-PARAM-11 | 回调注册 + 线程安全 | P1 | 2 | 2 |
| INFRA-PARAM-12 | C ABI 暴露 + 多语言 SDK 适配 | P1 | 3 | 2 |
| INFRA-PARAM-13 | profile 配置（搜索路径 + 热加载策略） | P1 | 1 | 2 |
| INFRA-PARAM-14 | 文档 + 示例 | P1 | 2 | 2 |
| **合计** | - | - | **32 点** | - |

## 风险与缓解

| 风险 | 缓解 |
|---|---|
| 自研 YAML 解析 bug | 优先用 header-only yaml-cpp（评估后定）；或限制 YAML 子集 |
| 配置引用循环 | 解析时检测循环，发现即 abort |
| 热加载引发竞态 | 用户回调责任 + 文档强调 |
| 多语言参数类型不一致 | C ABI 用 string 序列化，类型在 SDK 层转 |
| 配置文件包含敏感信息（密码） | 配合 redaction（与 logger 一致） |
| 文件 watch 不可用（mcu） | 编译期固定，无热加载 |

## 后续可能演进

- 如果未来需要远程配置中心 → 加 RemoteConfigSink（HTTP/gRPC）
- 如果未来需要 schema 验证 → 加 JSON Schema / CEL 表达式
- 如果未来需要配置 diff → CLI 加 `tianshu-ctl diff-params a.yaml b.yaml`
- 如果未来需要版本化 → 配置走 Git，每次启动 checkout

## 参考

- 用户参考：`apollo-base/docs/cyber/parameter.md`（本 ADR 设计直接对应）
- Apollo Cyber 参数实现（反面案例：gflags + 手动配置）
- gflags / glog（禁用，详见 ADR-0005）
- ROS 2 参数系统（参考其类型设计）
- 12-factor App：Config（环境变量优先）
- Kubernetes ConfigMap（外部配置参考）
