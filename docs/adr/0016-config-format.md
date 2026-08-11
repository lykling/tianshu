# ADR-0016：配置格式选型（TOML 主推 + YAML 兼容 + JSON 导入导出）

- **状态**：已接受
- **日期**：2026-08-10
- **决策者**：Pride Leong
- **关联**：[adr/0005](./0005-lightweight-multiplatform.md) · [adr/0009](./0009-doc-code-language.md) · [adr/0012](./0012-parameters.md)

---

## 背景

[ADR-0012 参数系统](./0012-parameters.md) 初版默认 YAML。用户反问"必须使用 yaml 格式吗？有建议使用什么格式更好吗？"。本 ADR 重新评估配置格式选型。

## 候选格式

| 格式 | 优势 | 劣势 |
|---|---|---|
| **TOML** | 严格语法、人类友好、Rust/Cargo/Python(pyproject) 主推 | 嵌套深的结构稍啰嗦 |
| **YAML** | 表达力强、超集（含 JSON）、K8s/GitHub Actions 主推 | 缩进敏感、易出 bug（如 Norway 问题）、解析复杂 |
| **JSON** | 标准、解析简单、工具链丰富 | 不能写注释、字符串必须引号、可读性差 |
| **JSON5 / JSONC** | JSON + 注释 + trailing comma | 标准化程度低 |
| **HOCON** | 人类友好、引用机制 | 小众、运维不熟 |
| **INI** | 极简 | 不支持嵌套、不支持数组 |

## 评估维度

### 1. 易读性（人类编写场景）

```
TOML:  ★★★★★（key=value 直观，section 用 [table]）
YAML:  ★★★★☆（缩进美，但易出 bug）
JSON:  ★★☆☆☆（引号 + 大括号噪音）
```

### 2. 严格性（解析错误率）

```
TOML:  ★★★★★（语法严格，错即报）
YAML:  ★★☆☆☆（缩进静默错误，"Norway" NO 被解析为 bool False）
JSON:  ★★★★★（严格，但 trailing comma 报错）
```

### 3. 表达力（嵌套 + 数组 + 引用）

```
YAML:  ★★★★★（最丰富）
TOML:  ★★★★☆（够用，深嵌套稍啰嗦）
JSON:  ★★★★☆（标准嵌套）
```

### 4. 解析依赖（与 ADR-0005 轻架构协同）

| 格式 | 自研工作量 | 第三方库 |
|---|---|---|
| TOML | 中（~500-1000 行） | toml++ (header-only, MIT) |
| YAML | 大（~3000+ 行） | yaml-cpp (MIT) |
| JSON | 小（~200 行） | nlohmann/json (header-only, MIT) / 自研 |

### 5. 工业先例

| 项目 | 配置格式 |
|---|---|
| Rust / Cargo | TOML |
| Python (pyproject.toml) | TOML |
| Go (Hugo) | TOML |
| Kubernetes | YAML |
| GitHub Actions | YAML |
| Docker Compose | YAML |
| Apollo Cyber | protobuf text + gflags |
| ROS 2 | YAML |

**趋势**：新项目（2018+）倾向 TOML，旧项目（K8s 时代）保留 YAML。

## 决策

**TOML 主推 + YAML 兼容 + JSON 仅导入导出**。

### 主推 TOML

```toml
# tianshu.toml（主配置文件）

[scheduler]
cpu_count = 4
big_core_count = 2

[sla]
deadline_ms = 50.0

[transport]
default_backend = "HYBRID"

[transport.shm]
pool_size_mb = 256

[lineage]
enable = true
ring_buffer_size = 1000

[[mainboards]]
name = "perception"
dag = "perception.dag"

[mainboards.params]
scheduler_cpu_count = 8
enabled_flows = ["perception_flow", "predict_flow"]

[[mainboards]]
name = "planning"
dag = "planning.dag"
```

### YAML 兼容（同等内容）

```yaml
# tianshu.yaml
scheduler:
  cpu_count: 4
  big_core_count: 2
sla:
  deadline_ms: 50.0
transport:
  default_backend: HYBRID
  shm:
    pool_size_mb: 256
mainboards:
  - name: perception
    dag: perception.dag
    params:
      scheduler_cpu_count: 8
      enabled_flows: [perception_flow, predict_flow]
```

### JSON 仅用于机器生成

```json
{
  "scheduler": {"cpu_count": 4},
  "mainboards": [...]
}
```

JSON 用于：

- `tianshu-ctl export-params` 输出（机器解析）
- 控制台 API 响应
- 跨语言交换（JSON 是最大公约数）

**不**用于人类编写。

## 加载策略

天枢启动时按以下顺序加载配置：

1. **搜索文件**（详见 [ADR-0012 §2](./0012-parameters.md)）：`tianshu.toml` → `tianshu.yaml` → `tianshu.json`
2. **第一个找到的生效**（不混合）
3. **显式指定**：`--config-file path/to/config.{toml,yaml,json}`

## feature flag（编译期可选）

为保持轻架构（[ADR-0005](./0005-lightweight-multiplatform.md)），三种 parser 都用 feature flag：

```bash
# CMake
cmake -B build \
  -DTIANSHU_WITH_TOML=ON \      # 默认 ON
  -DTIANSHU_WITH_YAML=ON \      # 默认 ON
  -DTIANSHU_WITH_JSON=ON        # 默认 ON

# 全部 OFF：参数仅来自 CLI + env + 代码默认值
cmake -B build \
  -DTIANSHU_WITH_TOML=OFF \
  -DTIANSHU_WITH_YAML=OFF \
  -DTIANSHU_WITH_JSON=OFF
```

**MCU profile**：默认全部 OFF（无文件系统）；参数编译期固定。

## parser 实现

| 格式 | 实现 | 依赖 |
|---|---|---|
| TOML | 自研极简（~1000 行，TOML 1.0 子集）或 [toml++](https://github.com/marzer/tomlplusplus)（header-only, MIT, ~5000 行） | 待 ADR-0024 决策 |
| YAML | 自研极简（不支持 YAML 全特性，只支持参数系统用到的子集）或 [yaml-cpp](https://github.com/jbeder/yaml-cpp)（MIT） | 待 ADR-0024 决策 |
| JSON | 自研极简（~200 行）或 [nlohmann/json](https://github.com/nlohmann/json)（header-only, MIT） | 待 ADR-0024 决策 |

**默认**：自研 TOML + nlohmann/json（轻量 header-only）；YAML 默认用 yaml-cpp。

## 修订 ADR-0012

ADR-0012 §2 配置方式修订：

| 来源 | 文件格式 | 备注 |
|---|---|---|
| CLI 参数 | 命令行字符串 | 不变 |
| 环境变量 | 字符串 | 不变 |
| launch 文件 | **TOML 主推**（兼容 YAML/JSON） | `.launch.toml` 或 `.launch.yaml` |
| 配置文件 | **TOML 主推**（兼容 YAML/JSON） | 详见本 ADR §加载策略 |

ADR-0012 §5 配置引用语法**不变**（`${var}` / `${env:VAR:-default}` / 算术表达式 / `import`），适用于所有格式。

## 影响范围

### 修订 INFRA-PARAM-5（详见 [ADR-0012](./0012-parameters.md)）

| ID | 描述（修订） | 估算 |
|---|---|---|
| INFRA-PARAM-5（修订） | TOML + YAML + JSON 三格式 parser（feature flag 控制） | 6（原 4） |

### 配置文件迁移

存量配置（如有）需要从 YAML 迁移到 TOML。提供 `tianshu-ctl convert-config input.yaml output.toml` 工具。

## 风险与缓解

| 风险 | 缓解 |
|---|---|
| TOML 表达力不够（深嵌套场景） | 切到 YAML（兼容） |
| 用户偏好 YAML 不接受 TOML | YAML 完整支持，文档不强推 TOML |
| 三 parser 都自研工作量大 | 自研 TOML 子集 + nlohmann/json（轻量）+ yaml-cpp（成熟） |
| 文档/示例混乱（不知道用哪个） | 默认示例用 TOML；YAML/JSON 标注"等价版本" |

## 后续可能演进

- 如果未来出现更强的人类友好格式（如 KDL）→ 评估替换
- 如果 schema 验证强需求 → 引入 `toml-validator` 或 `cerberus`
- 如果配置文件大 → 支持 `import` 多文件（详见 ADR-0012 §5）

## 参考

- TOML: https://toml.io/
- toml++: https://github.com/marzer/tomlplusplus
- yaml-cpp: https://github.com/jbeder/yaml-cpp
- nlohmann/json: https://github.com/nlohmann/json
- "Norway Problem": https://www.bram.us/2019/01/11/yaml-the-norway-problem/
