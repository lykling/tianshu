# ADR-0004：构建入口标准化（原生 bazel / cmake，禁 wrap 脚本）

- **状态**：已接受
- **日期**：2026-08-10
- **决策者**：Pride Leong
- **关联**：[adr/0003](./0003-build-system.md) · [02-development-plan.md](../02-development-plan.md)

## 背景

天枢决定 CMake + Bazel 双轨（[ADR-0003](./0003-build-system.md)）。**怎么调用**这两套构建系统同样需要决策。

参考 Apollo 项目的反例：Apollo 把 Bazel 包装为 `build_opt_gpu` / `build_opt_cpu` / `build_opt` 等 wrap 脚本，强制要求团队走 wrap（apollo-build skill 强制约束）。这导致：

- 团队成员离开 wrap 就不会构建（`bazel build //...` 直接调用反而陌生）
- wrap 脚本本身成为黑盒（CC / SYSROOT / 配置选项藏在脚本里）
- CI 必须复制 wrap 逻辑，复杂度高
- 新成员学习曲线被 wrap 拉长（先学 wrap 再学 Bazel）
- wrap 与原生 Bazel 行为漂移（wrap 修改后原生调用结果不一致）

## 候选方案

### 方案 1：wrap 脚本封装（Apollo 风格）

提供 `build_gpu.sh` / `build_cpu.sh` / `build_aarch64.sh` 等脚本，内部转调 bazel 加各种 flag。

**优点**：用户记一条命令；脚本可以塞复杂逻辑。
**缺点**：上面列出的所有 Apollo 痛点；wrap 与原生行为漂移；非标准。

### 方案 2：纯原生 bazel/cmake，flag 全靠用户记（**部分拒绝**）

只用 `bazel build //... --config=gpu --config=aarch64 --config=asan ...`，不提供任何封装。

**优点**：完全标准；零漂移风险。
**缺点**：用户要记一堆 flag；CI 配置冗长；新人门槛高。

### 方案 3：原生入口 + 分层配置文件覆盖（**已选**）

调用入口必须用**原生** `bazel` / `cmake`，但所有可覆盖的参数走分层配置文件：

- **Bazel**：`.bazelrc` 多 `--config=<name>` 段（`cpu` / `gpu` / `aarch64` / `qnx` / `asan` / `tsan` / `ubsan` / `release` / `debug`）
- **CMake**：`CMakePresets.json` 多 preset（与 bazel config 一一对应）
- **环境变量**：可选 `build.env` 文件，由 `.bazelrc` `import` 或 `CMakePresets.json` `cacheVariables` 加载，覆盖 `CC` / `CXX` / `SYSROOT` / `BAZEL_REMOTE_CACHE` 等

用户调用方式：

```bash
# Bazel
bazel build //...                      # 默认（debug + cpu + 当前架构）
bazel build //... --config=gpu         # GPU 目标
bazel build //... --config=aarch64     # 交叉编译 ORIN
bazel build //... --config=asan        # 启用 ASan
bazel build //... --config=release     # Release 配置
bazel build //... --config=gpu --config=asan   # 可叠加

# CMake
cmake --preset=default                 # 默认
cmake --preset=gpu                     # GPU
cmake --preset=aarch64                 # 交叉编译
cmake --build build/ --config Release

# 不允许：
# bash build_opt_gpu    ← ❌ 禁止 wrap 脚本
# ./build.sh gpu        ← ❌ 禁止 wrap 脚本
```

## 决策

**选方案 3**：原生 `bazel` / `cmake` 入口 + 分层配置文件覆盖，**禁止任何 wrap 脚本**。

## 决策依据

### 1. 标准化降低学习成本

任何有 Bazel / CMake 经验的人**第一天就能上手**，不需要先学项目专用的 wrap。开源用户、新成员、CI 配置都直接套用通用知识。

### 2. 避免 Apollo 痛点在天枢重演

天枢的存在意义之一就是摆脱 Apollo 包袱（详见 [ADR-0002](./0002-cyber-relation.md)）。如果构建工具重新走 Apollo 的 wrap 老路，与"完全独立实现"的精神违背。

### 3. 配置可版本化、可审计

- `.bazelrc` 和 `CMakePresets.json` 都在仓库内，git diff 可见
- 配置变更走 PR review，不是脚本黑箱改动
- CI 直接调原生命令，日志可读

### 4. 多场景覆盖无样板代码

不同场景（GPU/CPU/ARM/x86/Debug/Release/Sanitizer/CI）只是不同的 `--config=<name>` 组合，**不需要为每个场景写一个 shell 脚本**。Bazel/CMake 原生支持多 config 叠加。

### 5. 工程稳健性

- `.bazelrc` 是 Bazel 官方支持的配置文件机制，长期向后兼容
- `CMakePresets.json` 是 CMake 3.19+ 官方标准，IDE（VSCode/CLion）原生支持
- 环境变量文件机制（`build.env`）简单可移植，无魔法

### 6. 风险可控

- 复杂构建逻辑（如代码生成、protobuf）通过 Bazel `genrule` / `rules_*` 或 CMake `add_custom_command` 实现，**而不是**外部 shell 包装
- 用户忘记带 `--config` 时，默认配置 (`import %workspace%/.bazelrc.defaults`) 保证安全

## 配置覆盖机制细则

### `.bazelrc` 分层结构

```bazel
# .bazelrc - 仓库根目录

# 默认行为
common --enable_bzlmod
build --cxxopt=-std=c++20
build --copt=-Wall
build --per_file_copt=//tests//@gtest//...:-w

# 场景化配置（用户通过 --config=<name> 启用）
build:cpu       --define=TIANSHU_PROFILE=vehicle --define=TIANSHU_TARGET=cpu
build:gpu       --define=TIANSHU_PROFILE=vehicle --define=TIANSHU_TARGET=gpu --config=cuda
build:aarch64   --define=TIANSHU_PROFILE=vehicle --config=cross-aarch64 --cpu=aarch64
build:qnx       --define=TIANSHU_PROFILE=vehicle --config=cross-qnx --cpu=aarch64 --crosstool_top=//bazel/toolchains:qnx
build:embedded  --define=TIANSHU_PROFILE=embedded --copt=-DTIANSHU_PROFILE_EMBEDDED
build:mcu       --define=TIANSHU_PROFILE=mcu --config=cross-armv7m --copt=-DTIANSHU_PROFILE_MCU --copt=-ffreestanding --copt=-fno-exceptions --copt=-fno-rtti --config=embedded
build:server    --define=TIANSHU_PROFILE=server
build:desktop   --define=TIANSHU_PROFILE=desktop
build:asan      --config=sanitizer --define=SANITIZER=asan
build:tsan      --config=sanitizer --define=SANITIZER=tsan
build:ubsan     --config=sanitizer --define=SANITIZER=ubsan
build:debug     --compilation_mode=dbg
build:release   --compilation_mode=opt --copt=-DNDEBUG
build:cache     --disk_cache=//.bazel/cache
build:remote    --remote_cache=https://buildbuddy.internal

# CI 自动注入（不在仓库内）
try-import %workspace%/.bazelrc.ci
try-import %workspace%/.bazelrc.local
```

### `CMakePresets.json` 对齐

```json
{
  "version": 6,
  "cmakeMinimumRequired": {"major": 3, "minor": 23, "patch": 0},
  "configurePresets": [
    {"name": "default",  "displayName": "CPU x86_64 Debug", "binaryDir": "${sourceDir}/build/default"},
    {"name": "gpu",      "displayName": "GPU CUDA",         "binaryDir": "${sourceDir}/build/gpu", "cacheVariables": {"TIANSHU_TARGET": "gpu"}},
    {"name": "aarch64",  "displayName": "ARM ORIN",         "binaryDir": "${sourceDir}/build/aarch64", "toolchainFile": "${sourceDir}/cmake/toolchains/aarch64.cmake"},
    {"name": "asan",     "displayName": "ASan",             "binaryDir": "${sourceDir}/build/asan", "cacheVariables": {"CMAKE_BUILD_TYPE": "Debug", "SANITIZER": "asan"}}
  ],
  "buildPresets": [
    {"name": "default", "configurePreset": "default"},
    {"name": "gpu",     "configurePreset": "gpu"},
    {"name": "aarch64", "configurePreset": "aarch64"},
    {"name": "asan",    "configurePreset": "asan"}
  ]
}
```

### `build.env`（可选，机器/用户级）

```sh
# build.env - 不进 git（在 .gitignore），用户机器级配置
CC=/usr/bin/clang-16
CXX=/usr/bin/clang++-16
BAZEL_REMOTE_CACHE=https://buildbuddy.internal
TIANSHU_SYSROOT=/opt/orin/sysroot
```

加载方式：

- **Bazel**：`.bazelrc.local` 里 `import %workspace%/build.env`（通过 `action_env` 注入），或 `.bazelrc` 里 `build --host_action_env=TIANSHU_SYSROOT`
- **CMake**：`CMakePresets.json` 里通过 `cacheVariables` 引用环境变量，或在 `CMakeUserPresets.json` 中覆盖

## 等价性合约（与 ADR-0003 联动）

ADR-0003 的双系统等价性在这里进一步约束：

| 约束 | 校验 |
|---|---|
| `.bazelrc` 的 `--config=<name>` 与 `CMakePresets.json` 的 preset 一一对应 | 名字、语义、产出物都对齐 |
| 同一场景下两套系统产出可链接 | `libtianshu-gpu.so`（bazel）vs `libtianshu.so`（cmake）符号一致 |
| CI 同时跑 `bazel build --config=<X>` 和 `cmake --preset=<X>` | 任一失败阻塞 PR |

## 影响

### 仓库新增文件

```
tianshu/
├── .bazelrc               # 仓库内主配置（场景化 --config）
├── .bazelrc.ci            # CI 专用（由 CI 写入或仓库内预定义）
├── .bazelrc.local         # 用户本地覆盖（gitignore）
├── CMakePresets.json      # CMake 等价 preset
├── CMakeUserPresets.json  # 用户本地覆盖（gitignore）
├── build.env.example      # 模板（仓库内）
└── build.env              # 实际文件（gitignore）
```

### `.gitignore` 新增

```
.bazelrc.local
CMakeUserPresets.json
build.env
```

### 工作量影响

- INFRA-BUILD 新增 2 个功能点：
  - `.bazelrc` + `CMakePresets.json` 分层配置（含 CI/local 覆盖）
  - `build.env` 环境变量加载机制（Bazel 与 CMake 协同）
- INFRA-CI 不再为每个场景写独立 job 配置，而是统一调原生命令 + 不同 `--config`

### 用户文档

- README 必须用**原生 bazel/cmake 命令**作为示例，禁止出现 wrap 脚本
- quickstart 教用户"什么时候用 `--config=gpu`"，不是"什么时候跑 `build_gpu.sh`"

## 硬约束清单

1. ❌ **禁止**任何以 `.sh` / `.py` / `.bash` 结尾的"构建入口脚本"，包括但不限于：`build.sh` / `build_opt_gpu` / `tianshu-build` / `make.sh`
2. ❌ **禁止**通过 `make`（GNU Make 直接调用）作为构建入口（CMake 的 generator 是 ninja/make，但用户层调用是 `cmake --build`）
3. ✅ **允许**通过 Bazel `genrule` / `rules_cc` / `aspect_*` 或 CMake `add_custom_command` 实现复杂构建逻辑（这些是构建系统内的扩展点，不是入口封装）
4. ✅ **允许**通过 `.bazelrc` / `CMakePresets.json` / `build.env` 覆盖任何参数
5. ✅ **允许**工具脚本（`tools/format.sh` / `tools/lint.sh`），但这些是**辅助工具**，不是构建入口

## 后续可能演进

- 如果未来引入第三个构建系统（如 Buck2）→ 同样必须遵守本 ADR：原生入口 + 配置文件覆盖
- 如果某些场景必须用 wrap（极少数）→ 必须经过单独 ADR 审批，且 wrap 只能薄包装（直接转发原生命令 + flag）

## 参考

- Bazel `.bazelrc` 文档：https://bazel.build/run/bazelrc
- CMake Presets 文档：https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html
- 反面案例：Apollo `apollo-build` skill（强制 wrap 脚本）
