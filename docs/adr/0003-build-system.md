# ADR-0003：构建系统双轨（CMake + Bazel）

- **状态**：已接受
- **日期**：2026-08-10
- **决策者**：Pride Leong
- **关联**：[02-开发计划表.md](../02-开发计划表.md) · [adr/0002-cyber-relation.md](./0002-cyber-relation.md)

## 背景

天枢需要决定构建系统。候选：

| 系统 | 优势 | 劣势 |
|---|---|---|
| **CMake** | 开源生态通用、跨平台、IDE/clangd 友好、`compile_commands.json` 易得 | 大型 monorepo 增量构建慢、依赖管理弱 |
| **Bazel** | 增量构建快、依赖分析强、远程构建/缓存成熟、团队成员有 Bazel 经验 | 学习曲线陡、第三方依赖集成麻烦、IDE 支持相对弱 |

## 候选方案

### 方案 1：仅 CMake

**优点**：开源社区标准；IDE 友好；第三方库集成容易。
**缺点**：失去 Bazel 的增量构建速度（PoC 期编译频繁）；与团队既有 Bazel 工作流割裂。

### 方案 2：仅 Bazel

**优点**：与 Apollo 生态一致；增量构建快；远程构建/缓存（CI 加速）。
**缺点**：开源用户门槛高；IDE/clangd 集成复杂（需要 `bazel-clangd` 或 `hedron/bazel-compile-commands-extractor`）；放弃开源友好性。

### 方案 3：CMake + Bazel 双轨（**已选**）

**优点**：
- 开源用户用 CMake（低门槛 + IDE 友好）
- 团队内部用 Bazel（增量快 + 远程构建/缓存成熟 + 严格依赖分析）
- 同一份源代码两套都能构建（互为校验）

**缺点**：
- 维护成本翻倍（每新增模块要写两份 `CMakeLists.txt` + `BUILD.bazel`）
- 两套依赖管理要保持等价（容易漂移）
- CI 时间变长（除非做矩阵分摊）

## 决策

**选方案 3**：CMake + Bazel 双轨支持。

## 决策依据

### 1. 用户群体分层

- **开源用户**：偏好 CMake（IDE 友好、文档多、跨平台）
- **团队内部**：偏好 Bazel（增量快、远程构建/缓存成熟、严格依赖分析、团队成员有 Bazel 经验）
- 双轨让两类用户都满意

### 2. 团队既有 Bazel 工程经验可复用

团队成员有 Bazel 工作经验（Apollo / 其他大型 C++ 项目）。如果天枢只支持 CMake，团队需要切换工具链、丢掉既有 Bazel 经验积累（远程缓存配置、工具链、bzl 库等）。双轨让团队内部继续用熟悉的 Bazel，开源用户用 CMake。

### 3. 工程稳健性（互校验）

两套构建系统对同一份源代码出错的概率不同。定期互校验可以发现：
- CMake 漏掉的依赖（Bazel 严格依赖分析会暴露）
- Bazel 漏掉的 header（CMake 的 `target_include_directories` 全局可见会暴露）
- 平台特定差异（CMake 在 macOS/Windows 也能跑，扩展测试矩阵）

### 4. 工作量是真实成本但可摊薄

| 模块 | CMake 工作量 | Bazel 增量工作量 | 备注 |
|---|---|---|---|
| 顶层骨架 | 1 | 1 | workspace + 顶层 BUILD |
| 每个子模块 | 0.5 | 0.3 | BUILD 比 CMakeLists 稍紧凑 |
| 工具链文件 | 2 | 2 | aarch64 + QNX |
| 依赖管理 | 1 | 2 | bzl 体系学习成本 |
| CI 双跑 | - | 1 | 矩阵分摊 |
| **总计** | ~10 点 | **额外 ~7 点** | 总成本约 +70% |

但这是**一次性成本**，长期维护时大部分改动只发生在源代码，构建系统层面变化小。

### 5. 风险可控

- 模块设计上保持**单一职责**，避免两套系统出现不一致的复杂依赖图
- 用 `tools/sync-build-system.py` 脚本（待开发）校验两套系统目标集合等价
- CI 强制两套都跑通，避免漂移

## 影响

### 仓库结构

```
tianshu/
├── CMakeLists.txt              # CMake 顶层
├── cmake/                      # CMake 模块
├── WORKSPACE                   # Bazel workspace
├── BUILD                       # Bazel 顶层
├── bazel/                      # Bazel 工具链、宏、rules
├── tianshu/
│   ├── core/
│   │   ├── CMakeLists.txt
│   │   ├── BUILD.bazel
│   │   └── *.h *.cc
│   ├── dsl/
│   │   ├── CMakeLists.txt
│   │   ├── BUILD.bazel
│   │   └── ...
└── ...
```

### 命令等价对照

| 操作 | CMake | Bazel |
|---|---|---|
| 配置 | `cmake --preset=default` | （自动） |
| 编译 | `cmake --build build/default` | `bazel build //...` |
| 测试 | `ctest --test-dir build/default` | `bazel test //...` |
| 清理 | `rm -rf build/` | `bazel clean --expunge` |
| 安装 | `cmake --install build/default` | `bazel build //:tianshu_pkg`（pkg_tar） |
| 交叉编译 | `cmake --preset=aarch64` | `bazel build //... --config=aarch64` |
| 场景叠加 | `cmake --preset=gpu-asan`（preset 组合） | `bazel build //... --config=gpu --config=asan` |

> ⚠️ **构建入口约束**：必须用原生 `bazel` / `cmake`，**禁止 wrap 脚本**（如 `build_opt_gpu` / `build.sh`）。场景化覆盖通过 `.bazelrc --config=<name>` / `CMakePresets.json` / `build.env` 完成。详见 [ADR-0004](./0004-build-entry.md)。

### CI 矩阵

| 维度 | 值 |
|---|---|
| 构建系统 | CMake / Bazel |
| 编译器 | GCC 13 / Clang 16 |
| 架构 | x86_64 / aarch64（QEMU）/ armv7-m（QEMU，仅 mcu profile） |
| Sanitizer | ASan / UBSan / TSan（仅 hosted profile） |
| **Profile** | desktop / server / vehicle / embedded / mcu（详见 [adr/0005](./0005-lightweight-multiplatform.md)） |

矩阵组合 2×2×3×3×5 = 180，需要 job 分摊策略（如只在 PR 主分支跑全套，push 跑子集，nightly 跑嵌入式与 MCU profile）。

### 工作量影响

- Phase 0 任务 0.3（建构建系统）：1.5 → 3 周（含 Bazel 学习曲线）
- Phase 1 PoC：每模块新增 ~0.3 点的 Bazel 维护成本
- 长期：约 +10% 维护成本，换来开源友好 + 团队熟悉双轨收益

## 双系统等价性合约

为避免两套系统漂移，定义以下等价性硬约束（**入口约束详见 [ADR-0004](./0004-build-entry.md)**）：

| 约束 | 校验方式 |
|---|---|
| 目标集合等价 | CI 脚本对比 `bazel query //...` 和 CMake `add_library/executable` 列表 |
| 编译选项等价 | C++ 标准、警告等级、宏定义在两套系统必须一致 |
| 测试集等价 | `ctest` 与 `bazel test` 跑同一批用例 |
| 链接产出等价 | `libtianshu.so` 大小、符号表 diff 在合理阈值内 |
| **场景配置等价** | `.bazelrc --config=<name>` 与 `CMakePresets.json` preset 一一对应（详见 ADR-0004） |

## 后续可能演进

- 如果 Bazel 维护成本失控 → 退到方案 1（仅 CMake），保留 `BUILD.bazel` 文件做参考但不强制 CI
- 如果 CMake 在大模块上构建太慢 → 引入 `ninja` 替代 `make`（CMake 默认 generator）
- 如果未来引入 Conan / vcpkg 依赖管理 → 与 Bazel 的 `rules_foreign_cc` 协调
- 如果要支持 Buck2（Meta 新构建系统）→ 双轨扩展为三轨，需要重新评估

## 参考

- Bazel C++ 教程：https://bazel.build/start/cpp
- CMake + Bazel 双轨案例：https://github.com/envoyproxy/envoy（采用双轨）
- clangd 集成：https://github.com/hedronvision/bazel-compile-commands-extractor
