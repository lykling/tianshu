# ADR-0019：协程策略（Phase 1 回调调度 / Phase 2 C++20 stackless）

- **Status**: Accepted
- **Date**: 2026-08-11
- **Decision Maker**: Pride Leong
- **Related**: [adr/0001](./0001-dsl-form.md) · [adr/0005](./0005-lightweight-multiplatform.md) · [adr/0010](./0010-transport-shm-infra.md)

---

## Background

Phase 1 PoC 原计划实现 L4-CORO（ucontext + CRoutine + RoutineFactory），
对标 CyberRT 的协程调度。重新审视后发现天枢的调度模型不需要 stackful 协程。

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

天枢的调度模型（ADR-0001 codegen 产物）：

```
Scheduler → pick READY component → call Proc(msg) → Proc returns → pick next
```

- Proc() 是同步函数，跑完就返回
- "等待数据"不是在 Proc 内部 yield，而是调度器不调度它（state=WAITING）
- **不需要 stackful 协程**

**Phase 2: C++20 协程（Option 3），按需引入**

当 DSL 需要异步 I/O（网络请求、GPU 等待）时：

```cpp
Task<void> fetch_and_plan(Node& node) {
  auto data = co_await node.async_read(lidar);
  auto result = heavy_compute(data);
  co_await node.async_write(result);
}
```

## Rationale

### 为什么 Phase 1 不需要协程

| CyberRT 场景 | 天枢是否需要 |
|---|---|
| 组件从 Proc 内部深层 yield | ❌ 不需要（调度器控制 state） |
| 时间片抢占（协程超时 yield） | ❌ 不需要（Proc 跑完就返回） |
| 协程内等待 I/O | ❌ Phase 1 同步 I/O |
| 多协程并发执行 | ❌ 单线程调度即可（Phase 2 多线程） |

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
| L4-CORO-1 (汇编 swap) | P0, 4 点 | **跳过**（Phase 2 按需评估） |
| L4-CORO-2 (ucontext) | P0, 1 点 | **跳过** |
| L4-CORO-3 (CRoutine) | P0, 2 点 | **跳过** |
| L4-CORO-4 (RoutineFactory) | P0, 1 点 | **跳过** |
| L4-CORO-7 (MCU 禁用协程) | P1, 1 点 | **不需要**（Phase 1 无协程） |
| L4-SCHED-1..3 | 7 点 | **简化为回调版**（~3 点） |

**Phase 1 净减 ~8 点工作量**。

### 调度器简化

```cpp
// Phase 1 调度器（回调版）
class Scheduler {
 public:
  void add_task(std::string name, std::function<void()> fn);
  void run();  // loop: pick READY → call fn() → next
};
```

不需要 RoutineContext / CRoutine / RoutineFactory / ucontext / 汇编。

### Phase 2 演进路径

```cpp
// Phase 2: 新增 async 调度器（与回调版并存）
class AsyncScheduler {
  void add_task(Task<void> coroutine_task);
};

// 用户代码（DSL async 扩展）
node.on_input({camera}, [&](auto c) {
  co_await gpu_inference(c);  // C++20 协程
});
```

Phase 2 的 C++20 协程是**新增**，不是替换 Phase 1 的回调调度。
同步组件继续用回调，异步组件可选 C++20 协程。

## Future Evolution

- 如果 Phase 2 C++20 协程不足以满足需求（如需要 stackful yield）→ 评估汇编 swap（仅 vehicle profile）
- 如果 MCU 需要协程 → 评估 FreeRTOS task 作为协程后端
- 如果 C++23/26 标准化协程运行时 → 跟进标准
