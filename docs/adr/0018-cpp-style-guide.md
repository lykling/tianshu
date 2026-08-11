# ADR-0018：C++ 风格指南

- **状态**：已接受
- **日期**：2026-08-10
- **决策者**：Pride Leong
- **关联**：[adr/0007](./0007-api-spec-multi-language.md) · [adr/0009](./0009-doc-code-language.md) · [adr/0017](./0017-license.md)

---

## 背景

天枢是新项目，必须从 Phase 0 起就严格遵守统一的 C++ 风格规范，避免：

- 不同模块风格漂移（Apollo CyberRT 的痛点）
- Review 时争论风格（浪费时间）
- 历史包袱（后期想统一已经来不及）
- 工具链无效（IDE 提示不一致）

本 ADR 锁定 C++ 风格基线，所有源码必须通过 `clang-format` + `clang-tidy` 检查（零 WARNING，CI 强制）。

## 决策

### 1. 基础风格：Google C++ Style Guide

**为什么选 Google**：

| 候选 | 优势 | 劣势 | 选定 |
|---|---|---|---|
| **Google** | Apollo/Abseil/Protobuf 同风格，团队熟悉 | 列宽 80 偏窄 | ✅ |
| LLVM | clang 系项目标准 | 风格松散 | ❌ |
| Chromium | 浏览器项目主流 | 与中间件生态差异大 | ❌ |
| Mozilla | 老牌开源 | 风格过时 | ❌ |
| WebKit | Apple 系主流 | 与生态不符 | ❌ |

### 2. 与 Google 默认的差异（[`.clang-format`](../.clang-format)）

只列**与 Google 默认不同的项**：

| 项 | Google 默认 | TIANSHU 选择 | 理由 |
|---|---|---|---|
| `ColumnLimit` | 80 | **100** | 现代编辑器宽屏；80 太窄让代码碎 |
| `Standard` | c++11 | **c++20** | 项目用 C++20（详见 [ADR-0003](./0003-build-system.md)） |
| `DerivePointerAlignment` | true（按现有代码推断） | **false** | 锁定避免文件间漂移 |
| `PointerAlignment` | （由 Derive 决定） | **Left**（`int* p`） | 现代 C++ 主流（Stroustrup 推荐） |
| `AllowShortIfStatementsOnASingleLine` | WithoutElse | **Never** | 单行 if 易隐藏 bug |
| `AllowShortLambdasOnASingleLine` | All | **Inline** | 只允许短 inline lambda 单行，避免长 lambda 单行 |
| `AllowShortLoopsOnASingleLine` | false | **false** | 与 Google 一致 |
| `IncludeBlocks` | Preserve | **Regroup** | 强制重排 include，避免 reviewer 浪费时间 |
| `IncludeCategories` | （Google 自带） | **定制**（TIANSHU headers priority 4） | 让 TIANSHU 头文件分组建清晰 |

### 3. 命名规范（与 [ADR-0007 §命名规范](./0007-api-spec-multi-language.md) 一致）

| 类别 | 规范 | 示例 |
|---|---|---|
| 类型 / 类 / Struct / Enum | PascalCase | `Node`, `Reader`, `MessageFactory` |
| 函数 / 方法 | snake_case | `create_reader()`, `try_fetch()` |
| 成员变量（private/protected） | snake_case + 后缀 `_` | `deadline_ms_`, `priority_` |
| 成员变量（public） | snake_case（无后缀） | （尽量避免 public 成员） |
| 局部变量 / 参数 | snake_case | `channel_name`, `seq` |
| 常量 / 全局 const / constexpr | UPPER_SNAKE_CASE | `TIANSHU_VERSION_MAJOR`, `kDefaultQueueSize` |
| 宏 | UPPER_SNAKE_CASE | `TIANSHU_PARAM`, `TIANSHU_TRAITS_PROTOBUF` |
| 命名空间 | snake_case | `tianshu::core`, `tianshu::dsl` |
| 模板参数 | PascalCase + T 前缀 | `TMsg`, `TBackend` |
| Concept | PascalCase + Concept 后缀 | `MessageConcept`, `FlowBuilderConcept` |
| Enum 值 | kPascalCase | `kCpu`, `kGpu`（或 `kCuda`） |
| C ABI 函数 | `tianshu_<module>_<verb>_<noun>` | `tianshu_core_create_reader` |

**clang-tidy 自动检查**（[`.clang-tidy`](../.clang-tidy) 已配置）：

```yaml
CheckOptions:
  - key: readability-identifier-naming.ClassCase
    value: CamelCase
  - key: readability-identifier-naming.FunctionCase
    value: lower_case
  - key: readability-identifier-naming.VariableCase
    value: lower_case
  - key: readability-identifier-naming.PrivateMemberSuffix
    value: _
  - key: readability-identifier-naming.GlobalConstantCase
    value: UPPER_CASE
  - key: readability-identifier-naming.MacroDefinitionCase
    value: UPPER_CASE
```

### 4. 头文件规范

| 项 | 规范 |
|---|---|
| include guard | `#pragma once`（不用 `#ifndef _XXX_H_`） |
| 顺序 | 1. C 系统头文件 2. C++ 标准头文件 3. 第三方 .h 4. TIANSHU 头文件 5. 同目录头文件 |
| 路径 | 用 `#include "tianshu/xxx.h"`（不用相对路径 `../xxx.h`） |
| 前向声明 | 优先（减少编译期依赖） |
| 公开 vs 私有 | 严格分离（详见 [ADR-0007 §头文件组织](./0007-api-spec-multi-language.md)） |

### 5. 错误处理

| 边界 | 策略 |
|---|---|
| C++ 内部 | `std::expected<T, ErrorCode>`（C++23）/ `tl::expected`（C++20 过渡） |
| 构造函数 | 失败用 factory + `Result<T>`，不在构造函数抛异常 |
| 析构函数 | `noexcept`，绝不抛异常 |
| 公开 API（C ABI） | 整数错误码 + out-param，绝不抛 C++ 异常出 FFI 边界 |
| 测试代码 | 可以用异常（GTest EXPECT_THROW 等） |

### 6. 资源管理

- **强制 RAII**：所有资源（文件、锁、SHM、socket）必须用 RAII 类包装
- **禁用裸 new/delete**：用 `std::unique_ptr` / `std::make_unique`
- **禁用裸 malloc/free**：除非 C ABI 边界（且必须配对）
- **禁用 `std::shared_ptr` 默认**：只在真正需要共享所有权时用，且必须 `std::make_shared`
- **智能指针传参**：用 `T*` 或 `T&`，不直接传 `shared_ptr<T>`（除非要传递所有权）

### 7. 模板 / Concept

- **优先 Concept**：用 `requires` 表达约束（不靠 SFINAE）
- **Concept 命名**：PascalCase + Concept 后缀（`MessageConcept`, `FlowBuilderConcept`）
- **模板特化**：用户感知不到（用 trait class 隐藏）

### 8. const / constexpr / inline

- **const 一切能 const 的**：参数、返回值、成员函数
- **constexpr 优先**：编译期可计算的都用 `constexpr`
- **inline 小函数**：小于 5 行且性能敏感的函数用 `inline`

### 9. 注释规范（与 [ADR-0009](./0009-doc-code-language.md) 一致）

| 项 | 规范 |
|---|---|
| 语言 | **强制英文** |
| Doxygen | `///` 三斜线，英文 |
| 单行 | `//`，与代码空一格 |
| 块注释 | `/* */`，仅用于版权头或长说明 |
| TODO | `// TODO(<name>): <description>` 或 `// TODO(#issue): <description>` |
| FIXME | `// FIXME: <why>` + issue 号 |

### 10. 版权头（与 [ADR-0017](./0017-license.md) 一致）

所有 `.h` / `.cc` 文件必须有英文版权头：

```cpp
// Copyright 2026 The TIANSHU Team. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
```

### 11. 现代 C++ 特性使用

| 特性 | 推荐？ | 备注 |
|---|---|---|
| `auto` | ✅ | 局部变量类型推导；公开 API 不用 auto 返回 |
| 结构化绑定 | ✅ | `auto [a, b] = pair;` |
| Range-based for | ✅ | |
| `std::optional` | ✅ | |
| `std::variant` | ✅ | 替代 union + tag |
| `std::expected`（C++23） | ✅ | 错误处理首选 |
| Concept（C++20） | ✅ | 模板约束首选 |
| Coroutines（C++20） | ⚠️ | 用于 L4-CORO（详见后续 ADR-0011） |
| Ranges（C++20） | ✅ | |
| `<format>`（C++20） | ✅ | 替代 printf / iostream |
| `<span>`（C++20） | ✅ | 替代裸指针 + 长度 |
| 异常 | ⚠️ | 仅内部，不出 C ABI 边界 |
| RTTI（dynamic_cast） | ⚠️ | 仅测试代码用 |
| 多重继承 | ❌ | 禁用（接口类除外） |
| 菱形继承 | ❌ | 严禁 |
| 虚继承 | ❌ | 严禁 |
| 全局变量 | ❌ | 禁用（constexpr 常量除外） |
| 友元 | ⚠️ | 谨慎用，仅 operator<< 等 |
| C 风格 cast | ❌ | 用 `static_cast` / `dynamic_cast` |
| `goto` | ❌ | 严禁 |
| `using namespace std` | ❌ | 严禁 |
| `using namespace <xxx>`（全局） | ❌ | 函数内 `.cc` 文件可 |

### 12. 文件组织

```
include/tianshu/<module>/<file>.h     # 公开头文件
src/<module>/<file>.cc                # 实现
tests/<module>/<file>_test.cc         # 单测
```

- 一个 `.h` 对应一个 `.cc`（同名）
- 一个 `.h` 应该自包含（include 自己的所有依赖）
- 一个 `.cc` 第一个 include 是对应的 `.h`（验证头文件自包含）
- 测试文件命名为 `<source>_test.cc`

### 13. 函数 / 类设计

- **函数长度**：≤ 60 行（超 80 行考虑拆分）
- **参数数量**：≤ 5 个（超 7 个考虑 struct 封装）
- **圈复杂度**：≤ 15（clang-tidy `readability-function-cognitive-complexity`）
- **类大小**：≤ 500 行（超 1000 行考虑拆分）
- **单文件大小**：≤ 500 行（超 1000 行考虑拆分）

### 14. 性能规范（与 [ADR-0005](./0005-lightweight-multiplatform.md) 协同）

- **Hot path 禁用**：`new` / `delete` / `malloc` / `free`（用对象池）
- **Hot path 禁用**：`std::cout` / `printf`（用 logger，详见 [ADR-0011](./0011-logging.md)）
- **Hot path 禁用**：`std::endl`（用 `\n`，避免 flush）
- **Hot path 禁用**：`std::string` 拼接（用 `string_view` + `fmt::format`）
- **Hot path 优先**：`string_view` / `span` / 引用，不用值传递大对象

## CI 强制

| 检查项 | 工具 | 触发 | 失败行为 |
|---|---|---|---|
| 格式 | `clang-format --check` | 每个 PR | fail |
| 静态检查 | `clang-tidy --warnings-as-errors=*` | 每个 PR | fail |
| 编译警告 | `-Wall -Wextra -Wpedantic -Werror` | 每次 build | fail |
| Bazel build warnings | `grep "^WARNING:"` | 每个 PR | fail |
| CMake build warnings | `grep "warning:"`（过滤 CMake policy） | 每个 PR | fail |

详见 [.github/workflows/ci.yml](../.github/workflows/ci.yml) 的 lint job。

## 工具

| 工具 | 用途 |
|---|---|
| `tools/format.sh` | clang-format 包装 |
| `tools/tidy.sh` | clang-tidy 包装 |
| `tools/lint.sh` | 综合 lint（license + 非英文注释 + wrap 守护） |
| `.clang-format` | 格式配置（Google + 定制） |
| `.clang-tidy` | 静态检查配置 |
| `.clangd` | clangd IDE 配置（编译数据库路径） |

## 与现有 ADR 协同

| ADR | 协同点 |
|---|---|
| [adr/0005 轻架构](./0005-lightweight-multiplatform.md) | profile 编译选项；MCU 禁异常/RTTI |
| [adr/0007 多语言 SDK](./0007-api-spec-multi-language.md) | 命名规范（C ABI 函数）；公开/私有头文件分离 |
| [adr/0009 双语 + 英文 commit](./0009-doc-code-language.md) | 注释英文；commit Conventional Commits |
| [adr/0017 Apache-2.0](./0017-license.md) | 版权头 |

## 风险与缓解

| 风险 | 缓解 |
|---|---|
| 风格太严，开发摩擦 | 提供 `tools/format.sh` 一键修复；CI 错误信息可读 |
| clang-tidy 误报 | 在 `.clang-tidy` 用 `-check-name` 禁用特定检查 |
| 编译时间增加（含 -Werror） | 用 PCH / modules（Phase 1+ 评估） |
| CI lint job 时间长 | clang-tidy 增量检查（只跑改动文件） |

## 后续可能演进

- 如果 clang-tidy 检查过严 → 减检查项（每减一项要在 ADR 解释）
- 如果引入 fuzzing → 加 `clang-fuzz` CI job
- 如果引入 sanitizer → 在 CI 矩阵加 ASan/UBSan/TSan（已规划，详见 [INFRA-CI-3](../02-development-plan.md)）
- 如果未来切 Concepts 完全替代 SFINAE → 删除老 trait 类

## 参考

- Google C++ Style Guide: https://google.github.io/styleguide/cppguide.html
- clang-format 文档: https://clang.llvm.org/docs/ClangFormatStyleOptions.html
- clang-tidy 文档: https://clang.llvm.org/extra/clang-tidy/
- C++ Core Guidelines: https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines
- MISRA C++ 2008（工业参考）
- AUTOSAR C++ 14（车端参考）
