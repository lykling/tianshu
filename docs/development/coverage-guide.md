# Code Coverage Guide

## Clang source-based (recommended)

No D0 dead-code artifacts, no template overcounting. The right tool.

```bash
cmake --preset=coverage-clang
cmake --build --preset=coverage-clang
ctest --preset=coverage-clang

llvm-profdata merge -o tianshu.profdata build/coverage-clang/*.profraw

llvm-cov report -instr-profile=tianshu.profdata \
  -ignore-filename-regex='(googletest|googlebenchmark|gtest|gmock|_deps|\.cache|/usr/|bits/|ext/|c\+\+|tentative|test\.cc|hello_test)' \
  -object build/coverage-clang/bin/object_pool_test \
  -object build/coverage-clang/bin/cache_buffer_test \
  -object build/coverage-clang/bin/atomic_hash_map_test \
  -object build/coverage-clang/bin/spin_lock_test \
  -object build/coverage-clang/bin/rw_lock_test \
  -object build/coverage-clang/bin/sync_test \
  -object build/coverage-clang/bin/scheduler_test \
  -object build/coverage-clang/bin/node_test \
  -object build/coverage-clang/bin/intra_backend_test \
  -object build/coverage-clang/bin/hybrid_transport_test \
  -object build/coverage-clang/bin/transport_registry_test
```

HTML report:

```bash
llvm-cov show -instr-profile=tianshu.profdata \
  -format=html -output-dir=/tmp/tianshu_coverage_html \
  -ignore-filename-regex='(googletest|googlebenchmark|gtest|gmock|_deps|\.cache|/usr/|bits/|ext/|c\+\+|tentative|test\.cc|hello_test)' \
  build/coverage-clang/bin/object_pool_test \
  -object build/coverage-clang/bin/cache_buffer_test \
  -object build/coverage-clang/bin/atomic_hash_map_test \
  -object build/coverage-clang/bin/spin_lock_test \
  -object build/coverage-clang/bin/rw_lock_test \
  -object build/coverage-clang/bin/sync_test \
  -object build/coverage-clang/bin/scheduler_test \
  -object build/coverage-clang/bin/node_test \
  -object build/coverage-clang/bin/intra_backend_test \
  -object build/coverage-clang/bin/hybrid_transport_test \
  -object build/coverage-clang/bin/transport_registry_test
```

## GCC gcov (fallback)

```bash
cmake --preset=coverage
cmake --build --preset=coverage
ctest --preset=coverage

lcov --capture --directory build/coverage \
  --output-file /tmp/tianshu.info \
  --rc geninfo_auto_base=1 \
  --ignore-errors mismatch,inconsistent,negative \
  --filter function --demangle-cpp

lcov --extract /tmp/tianshu.info \
  '*/tianshu/include/*' '*/tianshu/src/*' \
  --output-file /tmp/tianshu_filtered.info

lcov --list /tmp/tianshu_filtered.info
```

`--filter function --demangle-cpp` merges GCC's D0/D1/D2 destructor
variants. Required for abstract base classes whose D0 (deleting
destructor) is unreachable dead code per
[Itanium C++ ABI issue #10](https://github.com/itanium-cxx-abi/cxx-abi/issues/10).

## Why two paths

| | Clang source-based | GCC gcov |
|---|---|---|
| Abstract D0 dead code | Does not exist | Needs `--filter function` |
| Template branch accuracy | Exact | Overcounts per-instantiation |
| Extra tooling | None | `--demangle-cpp` + `--filter` |
| Recommended | Yes | Fallback |

## Bazel

```bash
bazel coverage //... --config=clang --config=coverage-clang
```

## Targets

| Phase | Line | Function |
|---|---|---|
| Phase 1 (PoC) | >= 90% | >= 90% |
| Phase 2 (MVP) | >= 90% | >= 90% |
| Phase 3 (Cert) | >= 95% | >= 95% |
