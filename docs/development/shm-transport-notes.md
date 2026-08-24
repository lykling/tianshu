# SHM 跨进程传输 — 实现决策与陷阱记录

> **文档定位**：ADR-0010 决策 5 锁定了 SHM channel 跨进程传输的**架构决策**（段模型 / 初始化协议 / 唤醒 / 背压 / 生命周期）。本文是其**实现细节与踩坑记录**（错误注入测试、fork 覆盖率、工具链漂移等工程问题），供 Phase 2（ShmPool 通用分配器）和代码评审参考。
> **维护者**：Pride Leong
> **状态**：v1.0（2026-08）· 对应 commit `5e652a5` / `db32d60`
> **关联**：[adr/0010](../adr/0010-transport-shm-infra.md) · [adr/0013](../adr/0013-cross-machine-transport.md) · [evaluation/0002](../evaluation/0002-fork-shared-address-space.md)

---

## 总览

一条贯穿全部决策的主线：**共享内存里没有"进程"这个概念，只有物理页**。两个进程各自 mmap 同一 `/dev/shm` 对象时，内核返回的虚拟地址不同，但映射的是同一批物理页帧。因此一切跨进程协调——指针有效性、初始化互斥、slot 分配、唤醒、生命周期——都只能用「双方都能看到的内存 + 原子指令 / 内核同步原语」表达。

段布局（每 channel 一个命名段，约 2MB）：

```
[/dev/shm/tianshu_ch_<fnv1a(channel)>]
[SegmentHeader: magic + refcount + size]      ← ShmSegment 私有，用户不可见
[ChannelHeader: slots位图 + seq + 几何参数]     ← CAS 状态机: 0→INIT→READY
[slot 0..7: [pshared mutex+cond][SpscRing]]   ← 每 reader 独占一个
```

---

## 决策 1：ASLR 安全指针 — offset_ptr 存自相对偏移

**问题**：A 进程映射在 `0x7f4a...`，B 进程映射在 `0x7f9d...`。A 写入的绝对指针 `0x7f4a...1048` 在 B 侧解引用即 SIGSEGV。这与 PIE 的 R_X86_64_RELATIVE 重定位是同一个问题：数据里嵌的地址必须表示为「基址 + 加数」。

**方案**：存 `target - this`。两端点都在同一连续映射内时，映射基址漂移在减法中抵消。`get()` = 一次比较 + 一次加法。

**陷阱（已踩）**：move/copy 构造**不能直接复制 offset**。偏移的含义取决于 offset_ptr 对象自身的位置：位于 `0x1000`、offset=+8 的实例 move 到 `0x2000` 后，+8 指向错误地址。正确做法是四个操作（copy/move 的构造与赋值）全部从 raw target 重算；只有 0（null）是位置无关的。

**边界**：仅在单一连续映射内有效。跨段引用、或同段被分两次 mmap，偏移不变性破灭。Phase 2 ShmPool 若做多区段，需要区段 ID + 段内偏移的二级编码。

**验证**：两个进程 `grep tianshu /proc/<pid>/maps`，观察同一文件映射在不同 VA 区间。

## 决策 2：初始化互斥 — magic 字段兼作 CAS 状态机

**问题**：writer 和 reader 进程同时启动，都检查 `magic != READY` 后各自初始化——双方 `pthread_mutex_init` 覆盖同一 mutex（若对方线程已在等待则 UB）、双方清零 `live_slots`（已注册的 reader 永远不会被唤醒）。共享 mutex 无法解决：**初始化 pshared mutex 本身就需要同步**，鸡生蛋。

**方案**：裸内存上的原子 CAS 没有引导问题：

```
0（tmpfs 新页保证为零）─CAS─► INITIALIZING ──初始化完──► READY
                                 │ 输家：轮询等待（500µs × 最多 2s）
```

- `compare_exchange_strong(0 → INITIALIZING)` 只有一个进程能赢，独占初始化权
- 赢家 `magic.store(READY, release)` 发布；输家 `magic.load(acquire)` 看到 READY 时，happens-before 保证能看到赢家之前的**全部**初始化写入

**已知缺口**：赢家在 INITIALIZING 中途崩溃，段卡死，后来者 2s 超时失败。当前靠测试间清 `/dev/shm` 规避；Phase 2 需带时间戳的 stale 检测回收器。

## 决策 3：reader 注册 — slot 位图 fetch_or

**问题**：SpscRing 是单生产者单消费者，每个 reader 必须独占一个 ring。固定布局 `slot_stride` 寻址，但两个进程的 reader 怎么保证不抢同一个 slot？进程内锁帮不上忙——锁状态不在共享空间里。

**方案**：注册表本身放进共享段。`live_slots: atomic<uint32_t>`，8 bit 对应 8 slot。抢位 = `fetch_or(1 << i)`，一次 RMW 完成「测试 + 占位」。

- 对 tmpfs 共享页做原子 RMW 是安全的：两个映射别名同一物理页，x86 缓存一致性协议（MESI）保证 `lock cmpxchg` 跨进程跨核正确——与 Boost.Interprocess 在 SHM 里用 std::atomic 同一前提
- **为何固定 8 slot 而非动态列表**：动态列表需要跨进程分配器，而分配器自己又要共享状态——递归依赖。固定 = 零分配 + O(1) 寻址，代价是 8 readers/channel 上限（每 channel ~2MB = 8 × 256KB ring）。车辆拓扑典型扇出够用
- reader 抢到位后先 `reset_slot`（重建 ring、清读写指针），防上一个 reader 崩溃留下脏状态

## 决策 4：唤醒机制 — pshared condvar + MONOTONIC 超时兜底

**候选对比**：

| 方案 | 淘汰/采纳理由 |
|---|---|
| eventfd | 每 reader 一个 fd，跨进程传递要走 SCM_RIGHTS 或 fork 继承，fd 生命周期跨 exec 恶心 |
| 裸 futex | 理论最优（内核按物理页+偏移建等待队列，天然跨进程），但要自实现 condvar 协议（丢失唤醒/虚假唤醒/predicate），subtlety 太多 |
| **pshared condvar（采纳）** | glibc 的 `PTHREAD_PROCESS_SHARED` condvar 底层就是共享页上的 futex——让 glibc 替我们写对这个协议 |

**两个必须项**：

1. **必须有 mutex**。`pthread_cond_wait` 协议要求 mutex 保护谓词（"ring 非空或 stop_"）。没有 mutex 就是经典丢失唤醒：signal 先于 reader 进入 wait 到达，信号蒸发。
2. **必须 `condattr_setclock(CLOCK_MONOTONIC)`**。`pthread_cond_timedwait` 默认 CLOCK_REALTIME——墙钟一跳（NTP step），超时要么立刻触发要么永不触发。**车载环境这不是理论问题**（参考 ORIN phc2sys 时钟跳变事故）。

**为何 100ms timed wait 而非无限等**：析构路径。`~ShmReader` 置 stop_ 并 signal 一次，但若 signal 因任何边缘丢失（writer 崩溃、时序窗口），无限等待 = 线程 join 不上 = 进程退不出去。100ms 只在故障路径付出。

**数据可见性配对**：writer 先 push（payload → `write_pos.store(release)`）再锁 mutex + signal；reader 醒来 `write_pos.load(acquire)` 再读 payload。与决策 2 同一 happens-before 原则。

## 决策 5：环满背压 — 丢新消息（drop-new）

| 方案 | 后果 |
|---|---|
| 阻塞 writer | 迟钝 reader 把无限延迟注入生产者管线，同步 write() API 下不可接受 |
| 丢最旧（keep-latest） | 保新鲜度，适合 pose/track；但中途丢旧破坏 SPSC ring 的消息边界，复杂度陡增 |
| **丢新（采纳）** | 已接收消息的完整性与顺序不动；seq 出现空洞 = reader 端可观测的丢包计数器；push 返回 false 供 writer 计数 |

**修订计划**：cyber CacheBuffer / DDS KEEP_LAST 实际是丢旧保新鲜——为状态估计话题优化。L4-TRANS-8（QoS 配置）落地时，该策略应做成**每 channel 可配**，而非全局定死。

**ring 尾部 wrap 技巧**：消息布局 `[24B 头: size+seq+ts][payload 补齐到 8]`。消息跨不过物理尾部时，在尾部写 size=0 的 skip 标记，读写指针跳回环头；`pop()` 见 size==0 即知是标记。代价是尾部碎片，换来 memcpy 永远连续（不存在一条消息读两段）。

## 决策 6：段生命周期 — 三条泄漏路径

**Bug #0（调试时撞上的根因）**：早期版本 `data()` 返回映射基址，`ChannelHeader` 直接覆盖 offset 0 的 `SegmentHeader`（magic/refcount 互相践踏）→ refcount 永远到不了 0 → 永不 unlink。修复：SegmentHeader 私有化，`data()` 返回 `base + sizeof(SegmentHeader)`。**教训：残留段本身就是布局 bug 的证据链**——改布局后必须清 `/dev/shm` 再测。

**路径 (a) 静态 registry 持 strong ref**：进程内单例 map 持 `shared_ptr<ShmChannel>`，端点全关也不析构。修复：map 存 `weak_ptr`，取用 `lock()` 失败即惰性重建。段生命周期由**实际打开的端点数**决定。

**路径 (b) fork + `_exit()`**：`_exit()` 跳过所有析构，refcount 永不递减。修复：fork 测试的 child 在 `_exit()` 前用作用域块跑完析构。**已知残留风险**：fork 只克隆调用线程，child 里 `ShmReader::thread_` 的 pthread_t 指向内核侧不存在的线程，`join()` 严格说在 POSIX 层面是 UB（child 只该调 async-signal-safe 函数）。实测 glibc 下立即返回；硬化方向：`pthread_atfork` 子侧 detach，或重构为 fork 前不启动 reader 线程。

**路径 (c) 谁负责 shm_unlink**：unlink 只删名字，已有映射继续有效。规则：析构时 `fetch_sub` refcount，**最后一个解除映射者 unlink**。崩溃会留孤儿文件，而 `/dev/shm` 是 tmpfs = RAM——车上就是内存泄漏。Phase 2 需 reaper。

---

## 验收数据（fork benchmark，64B 消息）

| 指标 | 验收标准 | 实测 | 结论 |
|---|---|---|---|
| 跨进程吞吐 | ≥ 1M msg/s | 4.17M msg/s | 4 倍余量 |
| RTT 延迟（send→child 收→ack→parent 收） | < 1 ms | p50=58µs / p99=68µs | 17 倍余量 |

复现：`./build/desktop-clang/bin/shm_transport_benchmark [msgs] [samples]`

## 示例 API 用法

```cpp
// 进程 A                                          // 进程 B
Node node(TransportMode::kShm);                    Node node(TransportMode::kShm);
auto w = node.create_typed_writer<ImuData>("/imu"); auto r = node.create_typed_reader<ImuData>("/imu");
w->write(imu);                                      if (auto* imu = r->try_fetch()) { ... }
```

`try_fetch()` 是**取最新值**语义（不消费）：重复调用返回同一消息直到新消息到达。消费方去重需自行比对 `last_seq()` 变化（见 `examples/shm_listener.cc` 的 gap 计数——早期版本按每次轮询计数导致无符号下溢，报 2⁶⁴ drops）。

## 覆盖率：fork 子进程计数丢失的三个叠加坑

SHM 错误注入测试需要 fork + `_exit()`，期间发现子进程覆盖率计数静默丢失。三个独立原因叠加：

1. **弱符号陷阱**：`extern "C" void __gcov_dump(void) __attribute__((weak));` 在链接后是**弱未定义**——链接器从不为弱未定义符号从静态库拉成员，libgcov.a 里定义它的成员被跳过，运行时指针为 null，`if (p) p()` 静默 no-op。修法：CMake 在 coverage 构建下定义 `TIANSHU_HAVE_GCOV_DUMP`，改用**强声明**（非 coverage 构建仍无符号、不链接）。
2. **RLIMIT_FSIZE 自噬**：为注入 ftruncate 失败把软限设为 4096，而 gcda 文件 >4KB——子进程 dump 自己就会 EFBIG 失败。dump 前必须把 rlimit 复位为 RLIM_INFINITY。
3. **`_exit()` 跳过 atexit**：子进程必须手动 dump；同二进制写同一路径 gcda 时 libgcov 按 checksum 合并计数，父子数据自动求并集，无需 GCOV_PREFIX 分流。

Clang 构建同理：`__llvm_profile_write_file` 弱引用同样拉不进 profiling runtime（`TIANSHU_HAVE_LLVM_PROFILE_WRITE` 门控）。后续又发现三层 Clang 特有问题：(a) `LLVM_PROFILE_FILE` 的 `%p` 在 **fork 前已被 runtime 解析并缓存**，子进程 `__llvm_profile_write_file` 会写进父进程未来的文件、随后被覆盖——须先 `__llvm_profile_set_filename` 指向 pid 独立路径；(b) 无 `%m` 的固定文件名只落盘主可执行模块，DSO 计数不写；(c) 即便都做对，fork 子进程 dump 仍只含主模块——runtime 对多模块 dump 的限制，未解。因此 fsize 注入测试改为**进程内**完成（soft limit 降后升在硬限内合法），跨进程测试的子进程独占分支接受为 Clang 管线缺口，GCC 管线为权威数字来源。

## Phase 2 待办（本文档暴露的缺口）

- [ ] 初始化中途崩溃的 stale 段回收（magic + 时间戳 reaper）
- [ ] 环满策略每 channel 可配（L4-TRANS-8 QoS）
- [ ] kMaxReaders / ring 容量可配（当前硬编码 8 / 256KB）
- [ ] fork child 内线程 join 的 UB 硬化（pthread_atfork）
- [ ] offset_ptr 多区段支持（ShmPool 需要）
