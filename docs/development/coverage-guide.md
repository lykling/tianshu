# Code Coverage Guide

## Quick Start

```bash
# 1. Build with coverage instrumentation
cmake --preset=coverage
cmake --build --preset=coverage

# 2. Run tests (generates .gcda data files)
ctest --test-dir build/coverage

# 3. Capture, filter, and generate report
lcov --capture --directory build/coverage \
  --output-file /tmp/tianshu.info \
  --rc geninfo_auto_base=1 \
  --ignore-errors mismatch,inconsistent,negative \
  --filter function --demangle-cpp

lcov --extract /tmp/tianshu.info \
  '*/tianshu/include/*' '*/tianshu/src/*' \
  --output-file /tmp/tianshu_filtered.info

genhtml /tmp/tianshu_filtered.info \
  --output-directory /tmp/tianshu_coverage_html \
  --demangle-cpp

# 4. Open report
xdg-open /tmp/tianshu_coverage_html/index.html
```

## Text Summary

```bash
lcov --list /tmp/tianshu_filtered.info
```

## Why `--filter function --demangle-cpp`

GCC generates 3 destructor variants per virtual destructor:
- **D0** (deleting destructor): calls destructor body + `operator delete`
- **D1** (complete object destructor): calls destructor body
- **D2** (base object destructor): calls destructor body without virtual base handling

For abstract base classes, D0/D1 are unreachable dead code — the class
can't be instantiated, so `delete basePtr` dispatches to the derived
class's D0 via vtable, never the base's. This is acknowledged by the
[Itanium C++ ABI (issue #10)](https://github.com/itanium-cxx-abi/cxx-abi/issues/10)
and [LLVM](https://github.com/llvm/llvm-project/commit/47cc9db).

`--filter function --demangle-cpp` merges D0/D1/D2 at the same source
line into a single coverage entry after demangling. If any variant is
called (D2 always is), the destructor counts as covered. This is the
[lcov maintainer's recommended approach](https://github.com/linux-test-project/lcov/issues/79).

## Targets

| Phase | Minimum Line Coverage | Minimum Function Coverage |
|---|---|---|
| Phase 1 (PoC) | >= 90% | >= 90% |
| Phase 2 (MVP) | >= 90% | >= 90% |
| Phase 3 (Cert) | >= 95% (line + branch) | >= 95% |

## gcovr (alternative — known issues with GCC 15)

```bash
find build/coverage -name '*.gcda' -delete
ctest --test-dir build/coverage
gcovr --root . --filter 'tianshu/' --gcov-ignore-errors all \
  --txt-metric line --txt /tmp/tianshu_coverage.txt \
  --html-details /tmp/tianshu_coverage_html/index.html \
  build/coverage
```

> **Note:** gcovr 8.6 underreports template-heavy headers (e.g. `object_pool.h`)
> and abstract class D0 destructors when used with GCC 15. Use lcov for
> accurate results.
