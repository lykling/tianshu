# ADR-0009：文档与代码语言规范（双语文档 + 英文注释/commit）

- **状态**：已接受
- **日期**：2026-08-10
- **决策者**：Pride Leong
- **关联**：[adr/0003](./0003-build-system.md) · [adr/0004](./0004-build-entry.md) · [adr/0007](./0007-api-spec-multi-language.md) · [02-开发计划表.md](../02-开发计划表.md)

---

## 背景

天枢作为开源/内部跨语言项目（详见 [adr-0007](./0007-api-spec-multi-language.md) 多语言 SDK），需要明确"什么用中文、什么用英文、双语如何组织"。

| 关注点 | 现状问题 |
|---|---|
| 文档语言 | 当前全中文，国际化用户与开源生态被排除 |
| 代码注释 | 当前无规范，混合中英文风险高 |
| commit message | 当前无规范，混合中英文难追溯 |
| ADR / API 文档 | 当前全中文，与多语言 SDK 冲突 |
| 用户群体 | 团队内部中文；开源用户多语言；算法/研究团队国际化 |

## 候选方案

### 方案 1：全中文

文档/注释/commit 全中文。

**优点**：团队内部低门槛。
**缺点**：开源国际化失败；与多语言 SDK 冲突；commit log 国际工具链不友好。

### 方案 2：全英文

文档/注释/commit 全英文。

**优点**：完全国际化；开源生态友好。
**缺点**：团队内部学习曲线高；中文母语成员有摩擦；中文用户被排除。

### 方案 3：双语文档 + 英文注释/commit（**已选**）

- 文档：双语（中英文两个版本，互相对照维护）
- 代码注释：强制英文
- commit message：强制英文（Conventional Commits）
- API/类型/函数名：英文（详见 [adr-0007](./0007-api-spec-multi-language.md) 命名规范）

## 决策

**选方案 3**：双语文档 + 英文注释/commit。

## 文档语言规范

### 双语版本策略

每篇文档同时维护 `.zh.md`（中文版）和 `.en.md`（英文版）两个版本。

| 文件命名 | 含义 |
|---|---|
| `README.md` | 中文主版（默认入口） |
| `README.en.md` | 英文版 |
| `docs/zh/<file>.md` | 中文版 |
| `docs/en/<file>.md` | 英文版 |
| `docs/adr/NNNN-<slug>.zh.md` | 中文 ADR |
| `docs/adr/NNNN-<slug>.en.md` | 英文 ADR |

**特殊处理**：

- `README.md` 不带 `.zh.md` 后缀（GitHub 默认显示），保持中文主版；英文版用 `README.en.md`
- `LICENSE` / `.bazelrc` / `WORKSPACE` 等非文档文件不双语
- API 文档（Doxygen 生成）只英文

### 语言切换链接

每篇文档顶部必须有语言切换链接：

```markdown
> 中文 | [English](./00-overview.en.md)
```

英文版反之：

```markdown
> [中文](./00-overview.zh.md) | English
```

### 主版选择

| 文档类型 | 主版（默认） | 副版 |
|---|---|---|
| README | 中文（README.md） | 英文（README.en.md） |
| 用户指南 / Quickstart | **英文为主**（更国际化） | 中文（副） |
| ADR | **中英双语并重** | - |
| API 参考 | 英文 only | - |
| 内部设计 / 设计探讨 | 中文（团队母语） | 英文（副） |
| 法律 / 许可证 / 合规 | 中英双语（法律文本） | - |

**默认规则**：除非特别说明，新文档**中英双语并重**。

### 文档元数据

每篇文档顶部必须有 frontmatter（YAML 或 blockquote）：

```markdown
> **Title**: 00 - 方案总览 / Overview
> **Status**: Accepted
> **Maintainer**: Pride Leong
> **Last Updated**: 2026-08-10
> **Languages**: [中文](./00-overview.zh.md) | [English](./00-overview.en.md)
```

### 内容对齐

- 中英版本**语义对齐**（不是字字翻译，是意思一致）
- 数据 / 表格 / 代码块 / 引用 / 链接**完全相同**（不分语言版本）
- 标题与章节结构对齐
- 同步更新（一份改了，另一份必须 PR 同步）

### 翻译策略（针对存量文档）

| 文档 | 当前 | 处理 |
|---|---|---|
| `README.md` | 中文 | Phase 0 内翻译为 `README.en.md` |
| `docs/00-方案总览.md` ~ `02-开发计划表.md` | 中文 | Phase 0 翻译为 `.zh.md` + `.en.md`，原文件移到 `docs/zh/` 和 `docs/en/` |
| `docs/adr/0001-0008` | 中文 | Phase 0 翻译 |
| `docs/evaluation/0001-跨机通信评估.md` | 中文 | Phase 0 翻译 |
| 新增文档 | - | 必须双语 |

存量翻译工作量估算：~15 点（30 理想人日），见 [02-开发计划表 INFRA-DOC-6](../02-开发计划表.md)。

## 代码注释规范

### 强制英文

- 所有源代码注释、docstring、内联注释必须用英文
- 命名（变量/函数/类型）必须用英文（详见 [adr-0007](./0007-api-spec-multi-language.md)）
- 不允许 `// 中文注释` 或 `/* 中文 */`

### 例外

- **block comment** 解释业务背景时，可双语并列（先英文后中文）：
  ```cpp
  // SLA-aware scheduler that respects end-to-end deadlines.
  // 端到端 deadline 约束的调度器。
  ```
- **测试用例名 / 错误消息**：英文
- **TODO / FIXME / HACK**：英文，可附 issue 号

### C++ 文件头

所有 C++ 文件必须有英文版权 + 简短说明：

```cpp
// Copyright 2026 The TIANSHU Team. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// ...
//
// File: node.hpp
// Description: Public API for tianshu::core::Node - the factory for
//              readers/writers/services within a flow function.
```

### Doxygen / API 文档注释

英文 Doxygen：

```cpp
/// Creates a reader for the specified channel.
///
/// @tparam TMsg Message type satisfying MessageConcept (see adr-0008).
/// @param channel Channel name (e.g., "/perception/front").
/// @param opts Optional reader configuration (queue size, QoS).
/// @return Reader handle for use in on_input callbacks.
///
/// @note Thread-safe. Can be called concurrently from multiple threads.
template<MessageConcept TMsg>
Reader<TMsg> CreateReader(std::string_view channel, ReaderOptions opts = {});
```

## Commit Message 规范

### Conventional Commits

强制 [Conventional Commits](https://www.conventionalcommits.org/) 规范：

```
<type>(<scope>): <subject>

<body>

<footer>
```

**type** 必须是以下之一：

- `feat` 新功能
- `fix` bug 修复
- `docs` 文档
- `style` 代码风格（不影响功能）
- `refactor` 重构
- `perf` 性能优化
- `test` 测试
- `build` 构建系统
- `ci` CI 配置
- `chore` 杂项
- `revert` 回滚

**subject**：英文祈使句，< 70 字符，首字母小写，无句号

**body**：英文，解释为什么（why），不解释什么（what，diff 自带）

**footer**：关联 issue / PR / BREAKING CHANGE

### 示例

✅ 正确：

```
feat(l4-trans): add INTRA transport backend

INTRA backend enables zero-copy intra-process message delivery via
direct callback invocation. Required by Phase 1 PoC H1 (trace coverage).

Refs: L4-TRANS-2, #42
```

```
fix(dsl): handle empty reader list in on_input

Previously, on_input({}) would crash due to uninitialized DataVisitor.
Now returns early with a warning log entry.

Fixes: #128
```

❌ 错误：

```
更新 L4 transport 模块
```
（中文，无 type，无 scope）

```
fix: 修复了 bug
```
（无 scope，subject 无信息量）

```
FEAT: ADD NEW TRANS PORT
```
（全大写，typo）

## Pull Request 规范

- 标题：英文，遵循 Conventional Commits（同 commit）
- 描述：双语（先英文 summary，再中文详细）
- 关联 issue：英文
- 截图（如适用）：图注英文

模板：

```markdown
## Summary
<English summary, 1-3 sentences>

## Changes
- <Bullet list of changes, English>

## Test
- <How to test, English>

## Details (中文)
<Chinese detail explanation, optional>

## Related
- Closes #XXX
- Refs: <ADR / functional ID>
```

## Issue 规范

- **公开 issue**：英文标题 + 双语 body
- **内部 issue**：双语自由（看协作对象）

## CI 守护

### 文件命名 lint

- CI 检查 `docs/**` 下文件命名符合 `.zh.md` / `.en.md` 规范
- 检查中英版本成对出现（缺一份 PR fail）
- 检查语言切换链接存在且双向

### 注释语言 lint

- C++ 文件：`grep -P "[\x{4e00}-\x{9fff}]" --include="*.h" --include="*.cc"` 检查中文字符（除允许的 block comment 例外）
- Python/Rust/Go/Node 同理

### Commit message lint

- 使用 [commitlint](https://commitlint.js.org/) + Conventional Commits config
- CI 跑 commitlint 检查 PR 标题和所有 commit

### PR 模板

- CI 检查 PR 标题符合 Conventional Commits
- PR 模板自动注入双语模板

## 与其他 ADR 的协同

| ADR | 协同点 |
|---|---|
| [adr-0007](./0007-api-spec-multi-language.md) | API 命名 + 多语言 SDK 文档与本文档一致 |
| [adr-0005](./0005-lightweight-multiplatform.md) | profile 文档生成需双语 |
| [adr-0003](./0003-build-system.md) | 错误信息、配置文件名也走英文 |
| [adr-0004](./0004-build-entry.md) | 构建工具输出 / lint 信息英文 |

## 影响范围

### 新增 INFRA 功能点

| ID | 描述 | 优先级 | Phase |
|---|---|---|---|
| INFRA-DOC-6 | 现有文档双语化（README + docs + adr 翻译） | P1 | 0-1 |
| INFRA-DOC-7 | 文档模板（双语 README / ADR / 用户指南） | P0 | 0 |
| INFRA-DOC-8 | API 文档（Doxygen 英文）自动化 | P1 | 1 |
| INFRA-CI-10 | 文件命名 lint（中英成对 + 切换链接） | P0 | 1 |
| INFRA-CI-11 | 注释语言 lint（禁止中文注释） | P0 | 1 |
| INFRA-CI-12 | commitlint（Conventional Commits） | P0 | 0 |

### 文档目录重构

```
docs/
├── zh/                              # 中文文档
│   ├── 00-overview.md
│   ├── 01-roadmap.md
│   ├── 02-development-plan.md
│   ├── adr/
│   │   ├── 0001-dsl-form.md
│   │   └── ...
│   └── evaluation/
│       └── 0001-cross-machine-transport.md
├── en/                              # 英文文档
│   ├── 00-overview.md
│   ├── 01-roadmap.md
│   ├── 02-development-plan.md
│   ├── adr/
│   │   └── ...
│   └── evaluation/
│       └── 0001-cross-machine-transport.md
├── templates/                       # 文档模板（中英双语模板）
│   ├── README.zh.md
│   ├── README.en.md
│   ├── adr.zh.md
│   └── adr.en.md
└── adr/                             # 旧目录，Phase 0 后迁移完成删除
```

### README 文件

```
README.md            # 中文主版（GitHub 默认显示）
README.en.md         # 英文版
README.zh.md         # 软链接到 README.md（可选，方便语言切换链接对称）
```

## 风险与缓解

| 风险 | 缓解 |
|---|---|
| 翻译工作量大，挤占开发 | Phase 0 内一次性翻译；后续文档双语成为习惯 |
| 中英版本漂移（一份改了另一份忘改） | CI 强制成对出现；PR 模板提醒双语同步 |
| 团队中文母语者英文 commit 摩擦 | 提供 commit 模板 + 常用 type 速查 + 一周宽限期 |
| 翻译质量低（机翻痕迹） | 关键文档（README / ADR）人工精校；非关键文档允许机翻 + 渐进优化 |
| 中文术语映射不统一 | 建术语表（如 `调度器 = Scheduler`、`血缘 = Lineage`），加到 INFRA-DOC |

## 后续可能演进

- 如果国际化需求强烈 → 主版切换为英文（`README.md` 改英文，`README.zh.md` 中文）
- 如果未来支持日韩社区 → 加 `README.ja.md` / `README.ko.md`
- 如果出现 AI 翻译质量飞跃 → 引入自动翻译 CI 流水线
- 如果团队整体英文水平提升 → 内部 issue 也强制英文

## 参考

- Conventional Commits: https://www.conventionalcommits.org/
- Apache 项目双语惯例: https://www.apache.org/foundation/marks/
- CNCF 文档风格指南: https://github.com/cncf/foundation/blob/main/style-guide.md
- Doxygen: https://www.doxygen.nl/manual/docblocks.html
- commitlint: https://commitlint.js.org/
