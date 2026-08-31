# Changelog

All notable changes to TIANSHU are documented here.
Format based on [Keep a Changelog](https://keepachangelog.com/).
Versioning follows [Semantic Versioning](https://semver.org/).

## [Unreleased]

Phase 1 PoC — in progress.

### L4-PRIM — Data Structures (completed earlier in Phase 1)

- L4-PRIM-1..6: ObjectPool / CacheBuffer / AtomicHashMap / RWLock /
  SpinLock+TicketLock / BlockingCounter+Notification — all at 100% function
  coverage

### L4-SCHED — Scheduler

- L4-SCHED-1..3: callback-based Scheduler (priority queue + N workers, no
  coroutines per ADR-0019)

### L4-CORE — Node / Typed Messaging / DataFlow

- L4-CORE-5/6/7: DataVisitor (AllLatest fusion, 1-4 inputs) +
  DataDispatcher (channel_id -> buffer sinks, notify outside the lock) +
  DataNotifier; CacheBuffer gains type-erased CacheBufferBase::fill_bytes

- L4-TRANS-5: Message metadata (seq / timestamp / src_process_id / lineage_ptr)
- L4-CORE-1: MessageTraits<T> + POD auto-specialization
- L4-CORE-10: MessageConcept C++20 concept + TIANSHU_TRAITS_POD macro
- L4-CORE-2/3: typed Writer<T> / Reader<T> over transport layer
- L4-CORE-4: Node factory with typed create methods

### L4-TRANS — SHM Cross-Process Transport

- L4-TRANS-24: offset_ptr<T> — self-relative pointer, ASLR-safe (offset
  recomputed on copy/move; copying the raw offset is a latent bug)
- L4-TRANS-3: ShmSegment (shm_open/mmap RAII, payload-after-private-header,
  refcount + last-out-unlink) + SpscRing (lock-free SPSC byte ring with
  seq/timestamp metadata, wrap-marker, drop-on-full) + ShmChannel/ShmBackend
  (named segment per channel, 8 per-reader slots, atomic init state machine)
- L4-TRANS-4: wakeup via PTHREAD_PROCESS_SHARED condvar with
  CLOCK_MONOTONIC timed wait (100 ms fallback; immune to wall-clock steps)
- L4-TRANS-19: HybridTransport/Node TransportMode::kShm

Acceptance (fork-based benchmark, 64B messages):
- throughput 4.17M msg/s (bar: >= 1M) — 4x headroom
- RTT latency p50=58us p99=68us (bar: < 1ms)

Examples: shm_talker / shm_listener standalone cross-process demo binaries
(verified 35/35 messages, 0 dropped, ordered, zero /dev/shm residue).

### DSL op primitive — renamed from box, terminology aligned (ADR-0024)

- box -> op: executes ADR-0002's layer terminology (L1 = Operator,
  Component stays the L4 assembly-layer reuse unit); lifecycle shape
  has direct precedent in Kafka Streams Processor (init/process/
  forward <-> on_init/handle/publish)
- Rejected names recorded: block (reads as "blocking" in a real-time
  framework), actor (collides with traffic actors in AD), model (NN
  inference ambiguity)
- Multi-port op semantics locked in the ADR (AllLatest multi-input,
  per-output typed OpPub + tuple-of-chains return, per-output lineage
  = merged input branches + own hop; 0-input stays source/from
  territory); implemented on demand

### DSL box primitive (ADR-0024)

- builder.box<TIn, TOut>(chain, name, impl): read-write node with
  lifecycle — the DSL-layer projection of cyber's chassis/actuator
  Component. on_init publishes at wiring time (feedback loops bootstrap
  without seed sources — kills the ghost-state mispairing the demo
  workaround had); handle transforms inputs like a map
- BoxPub<T>: publish handle valid inside on_init/handle; lineage is
  map-identical from handle (input + hop) and source-identical from
  on_init (rooted at the box channel)
- builder.tap<T>(name): handle-only channel declaration — breaks
  cycles in feedback graphs (join references the port before the box
  writing it is constructed)
- on_init hooks run AFTER all wiring: every consumer mailbox of a box
  output is registered before the bootstrap publication
- full_chain_demo: chassis rewritten as ChassisMain box (seed source
  and plant map_to deleted); the graph has no chassis source at all —
  the box IS the chassis
- 2 new tests: box lifecycle lineage semantics (init root / handle
  derived, exact strings), feedback bootstrap without any seed source
  (convergence band + loop hops present)

### Full-chain closed-loop demo (sensors -> perception -> prediction -> planning -> control -> chassis -> feedback)

- full_chain_demo: 2 radars + GNSS -> radar fusion join -> perception
  join (3-branch lineage) -> prediction -> planning joins the chassis
  state (feedback) -> control -> stateful plant integrating speed ->
  writes BACK into the chassis channel; speed converges 0 -> ~20 m/s
- FlowChain::map_to(channel, fn): map with an explicit output channel —
  feedback edges (a stage writing into a channel that others join on);
  map/map_to marked const (chains usable as const handles)
- Lineage loop policy (v0.5.1): root-deduplicated merge (longer branch
  wins — the fresh loop-carrying copy), kMaxBranches=8 cap; branches
  stay constant in closed loops, loop traversal visible as hops
- Chassis channel feeds planning AND observability sinks
  (multi-consumer); closed-loop convergence guarded by a new test

### Lineage v0.5 branches + DSL join (ADR-0021/0022 amendments)

- Lineage branch model: roots-per-branch DAG provenance; join merges
  both parents' branch sets, subsequent hops close every branch
  ("a#1 -> x#1 -> j#0 | b#5 -> j#0"); linear chains render byte-identical
  to v0 (root()/hops() accessors preserved)
- Per-consumer lineage mailboxes replace the single side FIFO: publish
  fans a copy out to every stage registered on the channel — multiple
  sinks/joins on one channel no longer steal from each other
- FlowBuilder::join<A, B, C>(chainA, chainB, fn): AllLatest fusion over
  two streams (DataVisitor<A, B>, L4-COMP-6 semantics); chains can be
  held and composed (fan-out DAG declarations)
- 3 new tests: branch merge format, two-sinks-on-one-channel (both see
  every message with full lineage), join E2E with divergent source
  rates (branch root seqs verifiably different)

### kAuto transport selection (L4-TRANS-21, ADR-0023)

- AutoWriter dual-publishes (INTRA zero-copy fan-out + SHM broadcast,
  near-free with zero SHM readers); kAuto readers pick INTRA when this
  process hosts a real writer on the channel, else SHM — correct for
  ANY reader/writer creation order, no discovery service needed
- IntraChannelRegistry: register_writer marks real publishers;
  reader-created phantom entries do not count (has_writer)
- 4 new tests: same-process INTRA preference (synchronous delivery),
  reader-before-writer ordering, fork cross-process SHM fallback,
  phantom-writer immunity

### Cross-process schema sidecar (ADR-0020 Phase 2)

- Schema blob codec: encode_pod_schema / decode_pod_schema serialize the
  POD field table (magic + type name + per-field descriptors,
  little-endian); defensive parse rejects truncated or corrupted blobs
- DecoderRegistry::register_schema: runtime-owned tables (deque-backed
  name storage keeps FieldDesc::name pointers stable across moves);
  replaces prior entries for the same type name
- SHM sidecar segment /tianshu_schema_<fnv1a> beside the ring buffer
  (ADR option b: zero changes to existing segment layout, one page per
  schema'd channel, release/acquire publication, idempotent first-writer
  semantics, lifetime tied to the writer process)
- Node::create_typed_writer<T> auto-encodes the table when T has
  TIANSHU_TRAITS_POD_FIELDS; ShmBackend::create_writer publishes it
- MonitorApp::add_channel auto-loads the sidecar at attach and exposes
  ChannelView.schema_type_name; ti-monitor renders decoded fields with
  NO --decode flag (the flag still overrides)
- Verified live: shm_talker (typed writer) + ti-monitor --once in two
  separate processes auto-decode ImuData fields cross-process

### ti-monitor field decoding (ADR-0020 Phase 1)

- Field table: FieldDesc (name/offset/type/count) + FieldType scalars
  (double/float/i32/i64/u32/u64/bool) + inline arrays (up to 16 shown);
  decode_pod walks payload bytes defensively (schema drift -> skipped
  fields, never OOB)
- TIANSHU_TRAITS_POD_FIELDS macro: opt-in specialization +
  auto-registration in DecoderRegistry at static init (noexcept path);
  TIANSHU_FIELD helper emits the descriptor entry
- DecoderRegistry: type-name lookup (idempotent registration), decode()
  fills a format-neutral FieldTreeView
- ti-monitor --decode TYPE: renders "name = value" fields in the TUI
  detail pane and --once output; falls back to hex dump when the type
  has no table (cross-process schema distribution = Phase 2)
- Verified: 8 new unit tests + fork E2E (same-binary registry -> SHM
  frames -> decoded az=9.81); shm_talker authors ImuData fields

### L1-DSL / L2-LIN — Declarative flow + automatic lineage (ADR-0021/0022)

- DSL v0: FlowBuilder chained API (source/map/sink + with_sla slot),
  strongly-typed Stream<T> edges (wiring mistakes are compile errors),
  Flow declaration graph (the L1 compiler's IR input subset);
  FlowRuntime interpreter drives the L4 DataDispatcher directly
  (synchronous cascade per ADR-0021 amendment), absolute-deadline
  source pacing, lambda wiring deferred via detail::make_* (two-phase
  lookup: FlowRuntime is incomplete in flow.h)
- Lineage v0: root hop + cascade hops per message; DSL maps append
  hops automatically (zero user code); side-FIFO keyed by channel
  (single-writer/single-consumer v0 constraint); describe() renders
  "ch#seq -> ch#seq -> ..." chains
- demos: dsl_demo (20 Hz source -> double -> scale -> sink with
  lineage printing); 5 test cases (graph shape, cascade values,
  lineage chain exactness, SLA no-op)

### L4-COMP / L4-MAIN — Component framework + DAG launcher

- L4-COMP-1/2/3/10: ComponentBase lifecycle; Component<M, Out> and
  TwoInputComponent<M0, M1, Out> with AllLatest fusion via DataVisitor;
  TimerComponent (absolute-deadline scheduling, no cumulative drift);
  TimerSourceComponent (sensor-driver DAG entry); ComponentFactory +
  TIANSHU_REGISTER_COMPONENT
- L4-MAIN-1: ti + ti-launch (unified CLI per ADR-0002 terminology
  amendment; mainboard/ts/tsctl/tictl rejected with rationale);
  DagConfig INI-subset parser (TOML-shaped for ADR-0025); Launcher with
  reverse-order shutdown and signal handling
- Hello DAG milestone: source (10 Hz) -> doubler chain verified 15/15
  end to end through the full stack — the TIANSHU equivalent of cyber's
  first component DAG

### Build & Tooling

- Dual GCC+Clang: compiler-conditional coverage flags, desktop-clang /
  coverage-clang presets, bazel :clang / :coverage-* configs
- Coverage pipeline: lcov --filter function --demangle-cpp (official fix
  for abstract-class D0 dead code, Itanium ABI issue #10); Clang
  source-based coverage as the accurate path (no D0 artifact, no template
  overcounting)
- rules_cc bumped 0.0.17 -> 0.2.17 to match the resolved bzlmod graph

### Verification

- 239/239 CMake tests (Clang + GCC), 21/21 Bazel tests
- Line coverage 96.5%, function coverage 100% (lcov, filtered)
- Zero-warning build on both compilers

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
| 0019 | Coroutine strategy: Phase 1 callback / Phase 2 C++20 stackless |

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
