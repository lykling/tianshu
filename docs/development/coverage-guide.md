# Code Coverage Guide

## Quick Start (lcov — recommended for GCC ≥ 14)

```bash
# 1. Build with coverage instrumentation
cmake --preset=coverage
cmake --build --preset=coverage

# 2. Run tests (generates .gcda data files)
ctest --test-dir build/coverage

# 3. Capture and filter coverage data
lcov --capture --directory build/coverage \
  --output-file /tmp/tianshu.info \
  --rc geninfo_auto_base=1 \
  --ignore-errors mismatch,inconsistent,negative

lcov --extract /tmp/tianshu.info \
  '*/tianshu/include/*' '*/tianshu/src/*' \
  --output-file /tmp/tianshu_filtered.info

# 4. Generate HTML report
genhtml /tmp/tianshu_filtered.info --output-directory /tmp/tianshu_coverage_html

# 5. Open report
xdg-open /tmp/tianshu_coverage_html/index.html
```

## Text Summary

```bash
lcov --summary /tmp/tianshu_filtered.info
```

## gcovr (alternative — has known issues with GCC 15 template coverage)

```bash
find build/coverage -name '*.gcda' -delete
ctest --test-dir build/coverage
gcovr --root . --filter 'tianshu/' --gcov-ignore-errors all \
  --txt-metric line --txt /tmp/tianshu_coverage.txt \
  --html-details /tmp/tianshu_coverage_html/index.html \
  build/coverage
```

> **Note:** gcovr 8.6 underreports template-heavy headers (e.g. `object_pool.h`)
> when used with GCC 15. Use lcov for accurate results.

## Targets

| Phase | Minimum Line Coverage |
|---|---|
| Phase 1 (PoC) | ≥ 90% |
| Phase 2 (MVP) | ≥ 90% |
| Phase 3 (Cert) | ≥ 95% (line + branch) |

## How It Works

```
cmake --preset=coverage
  → adds --coverage flag (= -fprofile-arcs -ftest-coverage)
  → compiler emits .gcno files (coverage metadata)

ctest --test-dir build/coverage
  → test binaries run and write .gcda files (coverage data)

lcov --capture
  → reads .gcno + .gcda pairs
  → produces .info tracefile

genhtml
  → renders HTML from .info tracefile
```

## CI Integration (TODO)

```yaml
# .github/workflows/ci.yml (future)
- name: Coverage
  run: |
    cmake --preset=coverage
    cmake --build --preset=coverage
    ctest --test-dir build/coverage
    lcov --capture --directory build/coverage --output-file coverage.info \
      --rc geninfo_auto_base=1 --ignore-errors mismatch,inconsistent,negative
    lcov --extract coverage.info '*/tianshu/include/*' '*/tianshu/src/*' \
      --output-file coverage_filtered.info
- name: Upload to Codecov
  uses: codecov/codecov-action@v4
  with:
    file: coverage_filtered.info
```
