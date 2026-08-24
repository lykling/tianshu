# Code Coverage Guide

## Pipelines

| 管线 | 工具 | 定位 |
|---|---|---|
| GCC + lcov（`build/coverage`） | gcov/lcov | **权威管线**：含 fork 子进程计数，函数覆盖 100% |
| Clang source-based（`build/coverage-clang`） | llvm-profdata/llvm-cov | 快速参考：无 D0/模板伪影，但 fork 子进程的 .so 计数不落盘（见下） |

## GCC + lcov（权威）

```bash
cmake --preset=coverage
cmake --build --preset=coverage
ctest --test-dir build/coverage

lcov --capture --directory build/coverage --output-file /tmp/tianshu.info \
  --rc geninfo_auto_base=1 \
  --ignore-errors mismatch,inconsistent,negative \
  --filter function --demangle-cpp

lcov --extract /tmp/tianshu.info '*/tianshu/include/*' '*/tianshu/src/*' \
  --output-file /tmp/tianshu_filtered.info

lcov --list /tmp/tianshu_filtered.info                     # 文本汇总
genhtml /tmp/tianshu_filtered.info -o /tmp/tianshu_cov     # HTML
```

参数缺一不可：`--rc geninfo_auto_base=1`（路径解析）、`--ignore-errors mismatch,inconsistent,negative`（GCC 15 数据 quirk，缺失直接报错退出）、`--filter function --demangle-cpp`（合并 D0/D1/D2 析构变体，抽象类 D0 是 ABI 死代码，见 Itanium ABI issue #10）、`--extract`（滤掉 GoogleTest/标准库）。

当前基线：**97.6% line / 100% function**。未覆盖行均为 TOCTOU 竞态窗/资源耗尽路径，清单见 [shm-transport-notes](shm-transport-notes.md)。

## Clang source-based（参考）

```bash
cmake --preset=coverage-clang
cmake --build --preset=coverage-clang
ctest --preset=coverage-clang          # testPreset 注入 LLVM_PROFILE_FILE=%m.%p.profraw

llvm-profdata merge -o /tmp/t.profdata build/coverage-clang/*.profraw

llvm-cov report -instr-profile=/tmp/t.profdata \
  -ignore-filename-regex='(googletest|googlebenchmark|_deps|\.cache|/usr/|test\.cc|hello_test)' \
  -object build/coverage-clang/lib/libtianshu.so \
  $(ls build/coverage-clang/bin/*_test | sed 's/^/-object /')
```

要点：
- **`-object libtianshu.so` 必须带**：`src/*.cc` 的代码编译进共享库，其计数器在 profdata 里，但没有 .so 的 coverage mapping 就不会出现在报告里（漏掉它会少 ~40% 的行）
- **`%m.%p`**：`%p`（pid）让 fork 子进程与父进程各写各的文件——profraw 不合并计数，同名单文件后写者覆盖先写者
- HTML 版把 `report` 换成 `show -format=html -output-dir=...`

已知限制：fork 子进程调用 `__llvm_profile_write_file` 只落盘**主可执行模块**的计数，DSO（libtianshu.so）部分不落盘（runtime 对多模块 dump 的限制）。因此跨进程测试中仅子进程执行的行（如 `ShmReader::reader_loop` 部分分支）在 Clang 报告中缺失。需要完整数字时用 GCC 管线。

## Bazel

```bash
bazel test //...                        # 功能测试
bazel coverage //... --config=clang     # 覆盖率（LCOV 格式，bazel-out/_coverage/）
```

## Targets

| Phase | Line | Function |
|---|---|---|
| Phase 1 (PoC) | >= 90% | >= 90% |
| Phase 2 (MVP) | >= 90% | >= 90% |
| Phase 3 (Cert) | >= 95% | >= 95% |

## Troubleshooting

**系统工具链升级后 CMake 构建目录全部失效**（症状：`llvm-ar: No such file or directory`、部分测试崩溃、llvm-cov 报告行数骤减）。CMakeCache 缓存编译器/ar 的绝对路径，LLVM 升级（如 21→22）后路径死亡；且 ninja 半途失败会留下「新 .so + 旧测试二进制」的混合产物，旧二进制因 ABI 符号缺失批量崩溃。修复：

```bash
rm -rf build/coverage-clang build/desktop-clang   # 所有 clang 目录
cmake --preset=coverage-clang && cmake --build --preset=coverage-clang
```

Bazel 侧同理但症状不同（`cannot find 'ld'`）：`.bazelrc.local` 指向的 LLVM 路径要更新，且需 `bazel clean --expunge` 强制重新检测工具链。

**子进程覆盖率丢失（GCC）**：弱符号 `__gcov_dump` 不会让链接器拉取 libgcov 成员，运行时为 null 静默 no-op。coverage 构建定义了 `TIANSHU_HAVE_GCOV_DUMP`，测试里用强声明；dump 前若设置过 RLIMIT_FSIZE 需先复位，否则 gcda 写入自身 EFBIG。详见 [shm-transport-notes](shm-transport-notes.md)。
