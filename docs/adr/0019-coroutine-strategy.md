# ADR-0019：协程策略（Phase 1 回调调度 / Phase 2 C++20 stackless）

- **Status**: Accepted
- **Date**: 2026-08-11
- **Decision Maker**: Pride Leong
- **Related**: [adr/0001](./0001-dsl-form.md) · [adr/0005](./0005-lightweight-multiplatform.md) · [adr/0010](./0010-transport-shm-infra.md) · [adr/0012](./0012-parameters.md)

---

## Background

Phase 1 PoC 原计划实现 L4-CORO（ucontext + CRoutine + RoutineFactory），
对标 CyberRT 的协程调度。重新审视后发现天枢的调度模型不需要 stackful 协程。

两个关键问题驱动了本 ADR：
1. 协程的执行时机、时效性、优先级由谁来保证？
2. 组件订阅多通道或开多个异步任务时如何实现？

## Candidates Considered

### Option 1: ucontext + CRoutine (stackful)

对标 CyberRT。每个协程有独立 128KB 栈。

**Pros**: CyberRT 兼容；深层调用链可 yield。
**Cons**: POSIX only（macOS 弃用、Windows/MCU 无）；128KB/协程内存浪费；
H2 zero-overhead 验证更困难。

### Option 2: 汇编 swap (stackful)

CyberRT 生产环境用的方案。x86_64 + aarch64 手写汇编。

**Pros**: 最快（~100ns 切换）。
**Cons**: 每架构手写；维护成本高；MCU 不支持。

### Option 3: C++20 协程 (stackless)

语言级协程。编译器管理栈帧保存/恢复。~100B/协程。

**Pros**: 跨平台；零栈开销；现代 C++ 惯用法；编译器优化。
**Cons**: stackless（不能从深层调用链 yield）；MCU 编译器支持有限；
编程模型变化大（co_await/co_yield）。

### Option 4: 回调调度（无协程）

调度器直接调用 Component::Proc()，Proc 返回后选下一个。

**Pros**: 最简单；全 profile 兼容（含 MCU）；零额外依赖。
**Cons**: 不支持组件内部异步等待。

## Decision

**Phase 1: 回调调度（Option 4）**
**Phase 2: C++20 协程（Option 3），按需引入**
**Phase 3+: stackful 协程（Option 1/2），仅当生产数据显示需要故障隔离抢占时**

## Rationale

### 优先级和时效性保障（不依赖协程抢占）

天枢通过**三层纵深保障**确保优先级和时效性，均不依赖协程：

```
┌───────────────────────────────────────────────────────┐
│ 层 1：SLA / RTA（编译期，ADR-0012）                     │
│   编译期验证所有 deadline 可满足                         │
│   → 消除 80% 配置类问题                                 │
├───────────────────────────────────────────────────────┤
│ 层 2：任务优先级（调度器，L4-SCHED）                     │
│   调度器选最高优先级的 READY 任务先跑                     │
│   → 保证关键路径优先                                     │
├───────────────────────────────────────────────────────┤
│ 层 3：OS 线程调度（运行时）                              │
│   SCHED_FIFO + isolcpus + cpuset 绑核                   │
│   → OS 硬件级抢占保证（任意指令级抢占，不靠协程）          │
└───────────────────────────────────────────────────────┘
```

为什么 OS 线程调度比协程抢占更合适：

| 维度 | 协程抢占（stackful yield） | OS 线程调度（SCHED_FIFO） |
|---|---|---|
| 抢占粒度 | 仅在 yield 点 | **任意指令**（硬件中断） |
| 抢占延迟 | ~1μs | ~1-5μs（含内核调度） |
| 实现复杂度 | 高（ucontext/汇编 + 每协程 128KB 栈） | **低**（内核已有，只需配置） |
| MCU 兼容 | ❌ | ⚠️（FreeRTOS task 优先级） |
| 生产验证 | CyberRT | **所有 RTOS + Linux RT 系统** |

CyberRT 的 SchedulerChoreography 本质也是靠 OS 级别（cpuset + 线程绑定）保证时效，
协程只是任务包装层，不是时效性的核心保障。

### 多通道订阅（不需要协程）

组件订阅多个通道通过 DataVisitor + CacheBuffer + 融合策略实现，完全同步：

```cpp
void perception_flow(Node& node) {
  auto camera = node.reader<CameraMsg>("/camera");
  auto lidar  = node.reader<LidarCloud>("/lidar");
  auto imu    = node.reader<ImuData>("/imu");

  // AllLatest（默认）：camera 到达时取 lidar/imu 最新值
  node.on_input({camera, lidar, imu}, [&](auto c, auto l, auto i) {
    auto fused = fuse(c, l, i);
    output.write(fused);
  });
}
```

机制（使用已实现的 L4-PRIM CacheBuffer）：

```
camera 消息到达 → CacheBuffer<CameraMsg>.fill()
  → DataVisitor 检查：camera 有数据 AND lidar 有数据 AND imu 有数据？
    → 是：调度器标记组件 READY → 调用回调
    → 否：等待（组件不被调度，零开销）
```

### 异步任务（分阶段引入）

| 阶段 | 异步方式 | 需要协程？ |
|---|---|---|
| Phase 1 | 不需要（同步算子） | ❌ |
| Phase 2a | 线程池 + 回调 | ❌ |
| Phase 2b | C++20 协程（`co_await`） | ✅ stackless |
| Phase 3 | 批处理（`on_batch<N>`） | ❌ |

Phase 2b 的 C++20 协程用于解决 callback hell：

```cpp
// Phase 2a 回调方式（callback hell）
node.on_input({camera}, [&](auto c) {
  node.submit(detect_op, c, [&](auto det) {
    node.submit(predict_op, det, [&](auto pred) {
      output.write(pred);
    });
  });
});

// Phase 2b C++20 协程方式（扁平化）
node.on_input({camera}, [&](auto c) -> Task<void> {
  auto det = co_await node.async(detect_op, c);
  auto pred = co_await node.async(predict_op, det);
  output.write(pred);
});
```

多异步任务并行：

```cpp
node.on_input({camera}, [&](auto c) -> Task<void> {
  auto det_task   = node.async(detect_op, c);
  auto depth_task = node.async(depth_op, c);
  auto seg_task   = node.async(segment_op, c);

  auto det   = co_await det_task;
  auto depth = co_await depth_task;
  auto seg   = co_await seg_task;

  output.write(fuse(det, depth, seg));
});
```

### 为什么 Phase 1 不需要协程

| CyberRT 场景 | 天枢是否需要 | 原因 |
|---|---|---|
| 组件从 Proc 内部深层 yield | ❌ | 调度器控制 state（WAITING/READY） |
| 时间片抢占（协程超时 yield） | ❌ | OS 线程调度（SCHED_FIFO）更可靠 |
| TryFetch 失败时 yield | ❌ | 实际是 lambda return + state=WAITING（回调等价） |
| 协程内等待 I/O | ❌ Phase 1 | Phase 1 同步 I/O |
| 多协程并发执行 | ❌ | 多线程调度（Phase 2），不需要协程 |

### 为什么跳过 ucontext

| 问题 | 影响 |
|---|---|
| macOS 弃用 `makecontext` | 开发者体验差（需 pragma 抑制） |
| MCU 无 ucontext | ADR-0005 mcu profile 不兼容 |
| 128KB/协程栈 | 1000 个组件 = 128MB 栈内存 |
| H2 验证 | codegen 产物是普通函数调用，不涉及协程 |

### 为什么 C++20 协程推迟到 Phase 2

1. Phase 1 的 DSL codegen 产物是同步函数，不需要 async
2. C++20 协程的 promise_type 样板代码增加 Phase 1 复杂度
3. MCU 编译器对 C++20 协程支持不完善（Phase 3 再评估）
4. Phase 2 引入时，接口可以与回调版并存（用户可选）

## Impact

### Phase 1 工作量变化

| 任务 | 原计划 | 变化 |
|---|---|---|
| L4-CORO-1 (汇编 swap) | P0, 4 点 | **跳过**（Phase 3+ 按需评估） |
| L4-CORO-2 (ucontext) | P0, 1 点 | **跳过** |
| L4-CORO-3 (CRoutine) | P0, 2 点 | **跳过** |
| L4-CORO-4 (RoutineFactory) | P0, 1 点 | **跳过** |
| L4-CORO-7 (MCU 禁用协程) | P1, 1 点 | **不需要**（Phase 1 无协程） |
| L4-SCHED-1..3 | 7 点 | **简化为回调版**（~3 点） |

**Phase 1 净减 ~8 点工作量**。

### 调度器设计（回调版，Phase 1）

```cpp
struct Task {
  std::string name;
  TaskState state;           // READY / RUNNING / WAITING / DONE
  int priority;
  std::function<void()> fn;
};

class Scheduler {
 public:
  void add_task(std::string name, int priority, std::function<void()> fn);
  void run();  // loop: pick highest-priority READY → call fn() → next

  // DataNotifier 集成
  void mark_ready(const std::string& task_name);
  void mark_waiting(const std::string& task_name);
};
```

不需要 RoutineContext / CRoutine / RoutineFactory / ucontext / 汇编。

### Phase 2 演进路径

```cpp
// Phase 2: 新增 async 支持（与回调版并存）
// 回调组件继续用同步 Proc()，不受影响
// 异步组件可选 C++20 协程

node.on_input({camera}, [&](auto c) -> Task<void> {
  auto det = co_await node.async(detect_op, c);
  output.write(det);
});
```

Phase 2 的 C++20 协程是**新增**，不是替换 Phase 1 的回调调度。
同步组件继续用回调，异步组件可选 C++20 协程。

### Phase 3+ stackful 评估条件

仅当**生产数据**显示以下场景时才评估 stackful 协程：
1. 单个组件 Proc() 超时导致整个加载进程线程阻塞
2. SLA/RTA 无法在编译期预防的运行时 jitter
3. 需要从组件内部深层调用链强制 yield

此时仅在 vehicle profile 引入（汇编 swap 或 ucontext），其他 profile 继续用回调。

## Future Evolution

- Phase 2 C++20 协程 → 解决 async I/O 的 callback hell
- Phase 3+ 如果需要 stackful 抢占 → 评估汇编 swap（仅 vehicle profile）
- MCU → FreeRTOS task 优先级替代协程
- C++23/26 如果标准化协程运行时 → 跟进标准
