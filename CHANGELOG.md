# Changelog

All notable changes to TIANSHU are documented here.
Format based on [Keep a Changelog](https://keepachangelog.com/).
Versioning follows [Semantic Versioning](https://semver.org/).

## [Unreleased]

Phase 1 PoC — not yet started.

---

## Phase 0 — Foundation

### Build System

- INFRA-BUILD-1: CMake project skeleton (C++20, GCC 15+ / Clang 21+)
- INFRA-BUILD-3: C++20 standard + compiler requirements enforced
- INFRA-BUILD-4: compile_commands.json auto-generated + symlinked to repo root
- INFRA-BUILD-7: Bazel workspace (bzlmod MODULE.bazel, no WORKSPACE)
- INFRA-BUILD-8: Bazel module organization (per-module BUILD.bazel + rules_cc)
- INFRA-BUILD-9: Bazel toolchain + .bazelrc config layers (5 profiles + sanitizer + feature flags)
- INFRA-BUILD-11: External deps via bzlmod (rules_cc / rules_python / googletest / google_benchmark)
- INFRA-BUILD-15: .bazelrc layered config (build:cpu/gpu/aarch64/asan/tsan/release/...)
- INFRA-BUILD-16: CMakePresets.json (11 presets: desktop/release/server/vehicle/embedded/mcu/asan/...)
- INFRA-BUILD-18: Build entry guard (CI lint detects wrap scripts → fail)

### Dependency Governance

- INFRA-DEPS-1: ALLOWED_DEPS.txt whitelist (googletest / google_benchmark / rules_cc)
- INFRA-DEPS-2: Dependency application process (ADR + review + CI guard)

### Profile System

- INFRA-PROFILE-1: 5 profiles defined (desktop / server / vehicle / embedded / mcu)
- INFRA-PROFILE-2: TIANSHU_PROFILE_* conditional compilation macros

### CI

- INFRA-CI-1: GitHub Actions workflow (dual build matrix CMake + Bazel)
- INFRA-CI-2: clang-format + clang-tidy + license-header + no-wrap lint
- INFRA-CI-12: commitlint (Conventional Commits) + PR title lint

### Testing

- INFRA-TEST-1: GoogleTest 1.17.0 (Bazel bzlmod + CMake FetchContent)
- INFRA-TEST-2: GoogleMock (bundled with GoogleTest since 1.10+)
- INFRA-TEST-3: Test fixtures (gtest_discover_tests + cc_test with size="small")

### Benchmark

- INFRA-BENCH-1: GoogleBenchmark 1.9.x (Bazel bzlmod + CMake FetchContent)
- 3 microbenchmarks: BM_VersionMajor / BM_VersionString / BM_BuildProfile (~1.5 ns/op)

### Documentation

- INFRA-DOC-1: 4 docs (00-overview / 01-roadmap / 02-development-plan / README)
- INFRA-DOC-3: ADR template + process (18 ADRs published: 0001-0018)
- INFRA-DOC-7: Bilingual document template (per ADR-0009)
- 3 evaluation reports: cross-machine / ForkSHM / console

### API

- INFRA-API-2: C ABI design (version.h: extern "C" + opaque handle pattern)
- INFRA-API-3: Public/private header separation (include/tianshu/ vs src/)
- INFRA-API-4: Error code pattern (tianshu_status_t convention established)

### Library Skeleton

- tianshu/include/tianshu/version.h: C ABI version API (5 functions + profile macros)
- tianshu/src/version.cc: Version implementation
- examples/hello_world.cc: Smoke example (prints version + profile)
- tests/hello_test.cc: 6 gtest cases (version API correctness)
- benchmarks/version_benchmark.cc: 3 microbenchmarks

### Tooling

- .clang-format (Google base + project overrides: 100 col / C++20 / Left pointer)
- .clang-tidy (zero rule suppressions, WarningsAsErrors: '*')
- .clangd (compile database paths + inlay hints)
- .pre-commit-config.yaml (12 hooks: generic + conventional-commit + clang-format + tianshu-lint)
- .bazelignore (exclude CMake build/ from Bazel glob)
- tools/format.sh (clang-format wrapper)
- tools/tidy.sh (clang-tidy wrapper)
- tools/lint.sh (license-header + no-wrap + no-chinese-comments guard)

### Architecture Decisions (18 ADRs)

| ADR | Title |
|---|---|
| 0001 | DSL form: fluent builder + auto trace (JAX / torch.compile style) |
| 0002 | Independent reimplementation, API-compatible with Cyber RT |
| 0003 | Dual build system: CMake + Bazel |
| 0004 | Build entry standardization: native bazel/cmake, no wrap scripts |
| 0005 | Lightweight multi-platform: 5 profiles + dep governance + OSAL/HAL |
| 0006 | GPU acceleration: design ready, implementation Phase 2/3 |
| 0007 | Multi-language SDK: C ABI + Python/Rust/Go/Node |
| 0008 | Message format: FlatBuffers/Protobuf/POD with feature flags |
| 0009 | Bilingual docs + English-only code comments and commits |
| 0010 | Transport abstraction + SHM allocator + INTRA + offset_ptr |
| 0011 | Structured async logging |
| 0012 | Unified parameter system (4-source priority + hot reload) |
| 0013 | Cross-machine transport: Zenoh + MCU via Zenoh-pico |
| 0014 | Console: design complete, implementation Phase 3 |
| 0015 | Service discovery abstraction: DiscoveryBackend pluggable |
| 0016 | Config format: TOML primary + YAML fallback + JSON export |
| 0017 | License: Apache-2.0 |
| 0018 | C++ style guide: Google base + clang-format/tidy enforcement |

### Verification

- CMake: 32 targets built, 6/6 tests PASSED, zero warnings
- Bazel: 3 targets built, 1/1 tests PASSED, zero warnings
- clang-tidy: PASS (all clean, zero rule suppressions)
- clang-format: PASS
- pre-commit: 12 hooks all Passed

---

## 0.1.0 - 2026-08-10

Initial design baseline (18 ADRs + 3 evaluations + development plan).
Phase 0 engineering scaffold (dual build system + testing + CI + style).
