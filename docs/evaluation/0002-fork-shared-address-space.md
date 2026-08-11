# Fork 守护进程 + 共享地址空间跨进程零拷贝评估

> **文档类型**：技术评估报告（非决策 ADR）
> **决策状态**：⏳ 待用户拍板
> **维护者**：Pride Leong
> **日期**：2026-08-10
> **关联**：[adr/0005](../adr/0005-lightweight-multiplatform.md) · [adr/0007](../adr/0007-api-spec-multi-language.md) · [adr/0008](../adr/0008-message-format-multi.md) · [L4-TRANS](../02-开发计划表.md) · [evaluation/0001](./0001-cross-machine-transport.md)

---

## 1. 用户方案描述

通过 fork 守护进程保证多进程地址空间完全一致，从而支持复杂结构（含虚函数表）的跨进程零拷贝通信：

```
┌─────────────────────────────────────────────────────────┐
│  1. 启动第一个 mainboard                                 │
│     ↓                                                    │
│  2. mainboard 主动 fork 一个守护进程（daemon）           │
│     ↓                                                    │
│  3. daemon 在后台运行，作为 fork 模板                    │
│     ↓                                                    │
│  4. 启动第二个 mainboard：通知 daemon fork              │
│     ↓                                                    │
│  5. daemon fork 出第二个 mainboard 实例                  │
│     ↓                                                    │
│  6. 第二个 mainboard 继承 daemon 的地址空间             │
│     ↓                                                    │
│  7. 共享内存中存放的指针（含 vtable 指针）可直接解引用   │
│     ↓                                                    │
│  8. 最后一个 mainboard 退出时，daemon 一同退出           │
└─────────────────────────────────────────────────────────┘
```

## 2. 核心可行性判断

### 2.1 理论可行性

**结论：理论可行**。

Linux fork(2) 语义保证：

- 子进程是父进程的完整 COW 副本
- 子进程的虚拟内存布局与父进程**完全一致**（页表继承）
- vtable 在 C++ 对象的 `.rodata` 段，跟随可执行文件/`.so` 加载基址
- 如果两个进程 fork 自同一父进程，且 ASLR 关闭，**所有绝对地址（含 vtable）一致**

### 2.2 关键技术前提

| 前提 | 实现方式 | 默认状态 |
|---|---|---|
| **关闭 ASLR** | `personality(ADDR_NO_RANDOMIZE)` 系统调用，或父进程已 disable randomize | Linux 默认开启 ASLR，需显式关闭 |
| **关闭 PIE** | 编译时 `-no-pie -fno-pie` | GCC/Clang 默认开 PIE |
| **同一份 `.so`** | daemon 加载后所有 fork 子进程继承相同映射 | 需校验 |
| **加载顺序一致** | dlopen 顺序、LD_LIBRARY_PATH 一致 | daemon 控制即可 |
| **系统库版本一致** | glibc/libstdc++ 跨子进程同版本 | 部署约束 |

### 2.3 fork 后地址一致性的实证

**已验证**的等价场景：

1. **nginx master/worker**：worker 共享 master 的共享内存，但 nginx 严格只放 POD，不碰 vtable
2. **Apache prefork MPM**：同上
3. **PostgreSQL shared buffers**：postmaster fork backend，共享 SHM 含函数指针（手动注册，非 C++ vtable）
4. **Boost.Interprocess**：标准方案是用 offset_ptr，**不依赖地址一致**
5. **Iceoryx**（Bosch 开源，Apollo 用）：同样用 offset_ptr，**不依赖地址一致**

**结论**：fork 方案在工业界有先例，但**现代主流 SHM 库都不依赖地址一致**，原因详见下文风险分析。

## 3. 详细风险分析

### 3.1 安全风险

| 风险 | 严重度 | 说明 |
|---|---|---|
| ASLR 关闭 | **高** | 整个进程暴露在 ROP/heap overflow 攻击面；攻击者知道代码/数据准确地址 |
| PIE 关闭 | **高** | 二进制与 ASLR 协同防护失效；同样暴露 |
| 守护进程权限 | **高** | daemon 通常需 root 或高权限（fork 任意 user 进程） |
| 车端安全标准冲突 | **高** | ISO 21434 / UN-R155 要求 ASLR；关闭可能不合规 |

### 3.2 部署严苛性

| 约束 | 说明 |
|---|---|
| 同一份 `.so` | 所有 mainboard 必须用同一份编译产物，hash 校验 |
| 相同加载顺序 | 任何动态库加载顺序差异 → 地址偏移 → 解引用崩溃 |
| `LD_LIBRARY_PATH` 一致 | 环境变量影响 |
| glibc/libstdc++ 同版本 | OS 升级可能破坏 |
| 同机部署 | fork 不能跨机，与 Zenoh 跨机方案割裂 |
| 容器化困难 | Docker/containerd 默认 seccomp 禁 `personality(ADDR_NO_RANDOMIZE)` |

### 3.3 单点故障

| 风险 | 缓解 |
|---|---|
| daemon 崩溃 → 无法 fork 新 mainboard | 需 daemon 自动重启；但重启后地址不一致，老进程受影响 |
| daemon CPU/内存泄漏 | 长期运行风险 |
| daemon 死锁 | 监控复杂 |

### 3.4 跨语言不友好（与 [adr-0007](../adr/0007-api-spec-multi-language.md) 冲突）

| 语言 | 能否用此机制 | 说明 |
|---|---|---|
| C++ | ✅ | 原生支持 |
| Python | ❌ | Python 解释器无法跨进程共享 vtable |
| Rust | ⚠️ | Rust 没有 vtable 概念，但 trait object 有等价物，跨进程不安全 |
| Go | ❌ | Go runtime 自管内存，fork + 地址一致不可靠 |
| Node.js | ❌ | V8 自管内存，完全不可用 |

**结论**：仅 C++ 能用此机制；多语言 SDK 只能走 C ABI 包装，但包装后失去零拷贝优势。

### 3.5 复杂对象的限制

即使地址一致，**含堆分配的对象仍需特殊处理**：

```cpp
// ❌ 不能跨进程零拷贝（堆指针跨进程无意义）
class BadMessage {
  std::string payload;          // 内部堆指针
  std::vector<float> data;      // 内部堆指针
  std::shared_ptr<Context> ctx; // 控制块跨进程不安全
};

// ✅ 可以跨进程零拷贝（POD 或固定布局）
struct GoodMessage {
  float data[1024];             // 固定大小数组
  uint64_t timestamp;
  int sensor_id;
};

// ⚠️ 含 vtable 但内部无堆指针：理论上可，但工程上脆弱
class PureVirtualMessage {
 public:
  virtual ~PureVirtualMessage() = default;
  virtual void process() = 0;
  // 无成员变量或成员全 POD
};
```

**实践约束**：用户对象必须是 **POD 或 trivially-copyable layout**（含 vtable 但无堆指针），与 ADR-0008 的 POD trait 形成子集。

### 3.6 调试与可观测性

| 维度 | 影响 |
|---|---|
| gdb 多进程调试 | 复杂（follow-fork-mode） |
| core dump 分析 | 跨进程引用难追溯 |
| 内存泄漏排查 | valgrind 跨进程困难 |
| 性能 profiling | perf 仍可用，但 flamegraph 跨进程合并困难 |
| ASAN/TSAN | 多 fork 场景支持有限 |

### 3.7 与现有 ADR 的冲突

| ADR | 冲突点 |
|---|---|
| [adr-0005 轻架构](../adr/0005-lightweight-multiplatform.md) | ASLR 关闭违反嵌入式安全规范；mcu profile 不可用（无 fork） |
| [adr-0007 多语言 SDK](../adr/0007-api-spec-multi-language.md) | 仅 C++ 可用，与多语言 SDK 冲突 |
| [adr-0008 消息多格式](../adr/0008-message-format-multi.md) | FlatBuffers 已经零拷贝（builder buffer memcpy），与 fork 方案重复 |
| [adr-0009 双语 + 英文 commit](../adr/0009-doc-code-language.md) | 无直接冲突 |
| Profile 资源预算 | daemon 额外内存占用（~10-50MB） |

## 4. 改良方案设计

如果用户接受上述风险并希望落地，**改良版**方案如下：

### 4.1 总体架构

```
┌──────────────────────────────────────────────────────────┐
│                     用户态                                 │
│                                                            │
│   mainboard #1     mainboard #2     mainboard #3           │
│   (worker)         (worker)         (worker)               │
│       │                │                │                  │
│       └──── IPC ───────┴──── IPC ───────┘                 │
│                       ↓                                    │
│              ┌────────────────────┐                        │
│              │ tianshu-fork-daemon │                        │
│              │ (prefork template) │                        │
│              └────────────────────┘                        │
│                       ↓                                    │
│  ┌─────────────────────────────────────────┐              │
│  │ SHM Region: TIANSHU_SHARED_ADDRESS_SPACE │              │
│  │ - placement new allocator               │              │
│  │ - hash-verified binary layout           │              │
│  │ - cross-process vtable dereference      │              │
│  └─────────────────────────────────────────┘              │
│                                                            │
└──────────────────────────────────────────────────────────┘
                          ↓
                    内核：fork(2) + personality()
```

### 4.2 daemon 生命周期

```cpp
// tianshu-fork-daemon 主流程（伪代码）

int main() {
  // 1. 关闭 ASLR（仅 daemon 进程，子进程继承）
  if (personality(ADDR_NO_RANDOMIZE) == -1) {
    // 降级：log warning，继续但禁用 ForkSHM mode
  }

  // 2. 初始化 tianshu 运行时（加载所有 .so 到固定基址）
  tianshu::Init(/* load_standard_libs= */ true);

  // 3. 计算二进制布局 hash
  BinaryLayoutHash hash = compute_binary_layout_hash();
  shm_write_hash(hash);

  // 4. 创建 unix domain socket 监听 fork 请求
  int sock = bind_fork_socket();

  // 5. 注册 sigchld handler（监控子进程生命周期）
  signal(SIGCHLD, on_child_exit);

  // 6. 主循环：等待 fork 请求
  while (running) {
    auto req = accept_fork_request(sock);
    pid_t child = fork();
    if (child == 0) {
      // 子进程：执行 mainboard 入口
      close(sock);  // 子进程不监听
      exec_mainboard(req.argv, req.envp);
    } else {
      // 父进程：记录子进程
      children[child] = req;
    }
  }

  // 7. 等所有子进程退出后退出
  while (!children.empty()) wait();
}
```

### 4.3 mainboard 启动流程（改造后）

```cpp
// mainboard main（伪代码）

int main(int argc, char** argv) {
  // 1. 解析参数
  auto opts = parse_args(argc, argv);

  // 2. 检测是否启用 ForkSHM mode
  if (opts.fork_shm_mode) {
    // 3. 通过 unix domain socket 请求 daemon fork
    auto response = request_fork_from_daemon(opts);
    if (!response.success) {
      // 降级到普通启动
      log_warning("ForkSHM mode unavailable: " + response.reason);
      opts.fork_shm_mode = false;
    } else {
      // daemon 已经 fork 了我们，我们就是那个子进程
      // 当前 mainboard 进程的地址布局与 daemon 一致
    }
  }

  // 4. 校验 SHM 中的 binary layout hash
  if (opts.fork_shm_mode) {
    BinaryLayoutHash my_hash = compute_binary_layout_hash();
    if (my_hash != shm_read_hash()) {
      fatal("Binary layout mismatch with daemon; ForkSHM mode disabled.");
    }
  }

  // 5. 启动 mainboard 正常逻辑
  return mainboard_main(argc, argv);
}
```

### 4.4 SHM allocator（placement new）

```cpp
namespace tianshu::shm {

// SHM 区域分配器（线程安全，多进程共享）
class ShmAllocator {
 public:
  static ShmAllocator& instance();

  // 分配：在 SHM 区域分配 N 字节，返回 SHM 内偏移
  void* allocate(size_t bytes, size_t alignment = alignof(std::max_align_t));

  // 释放
  void deallocate(void* ptr);

  // 在 SHM 中构造对象（placement new）
  template<typename T, typename... Args>
  T* construct(Args&&... args) {
    void* mem = allocate(sizeof(T), alignof(T));
    return new (mem) T(std::forward<Args>(args)...);
  }

  // 在 SHM 中销毁对象
  template<typename T>
  void destroy(T* obj) {
    obj->~T();
    deallocate(obj);
  }
};

}  // namespace tianshu::shm
```

### 4.5 跨进程 vtable 解引用（设计契约）

```cpp
// 用户定义：可跨进程的"含 vtable 但无堆指针"对象
class CrossProcessHandler {
 public:
  virtual ~CrossProcessHandler() = default;
  virtual void process(const SensorData& data) = 0;
  virtual const char* name() const = 0;
  // 注意：不允许有 std::string/std::vector 等含堆指针的成员
 private:
  // 只允许 POD 成员
  int handler_id_;
  float threshold_;
};

TIANSHU_MARK_CROSS_PROCESS_SAFE(CrossProcessHandler);
// 该宏在编译期 static_assert 检查：
// - 所有成员是 POD 或 CrossProcessSafe 标注
// - 无 std::string / std::vector / std::shared_ptr
// - 虚函数表无 RTTI 依赖（禁 dynamic_cast 跨进程）
```

### 4.6 启用条件（Profile-aware）

| Profile | ForkSHM mode 默认 | 备注 |
|---|---|---|
| desktop | 关闭（开发不强制 ASLR 关） | 可手动开 |
| server | 关闭（容器化部署） | 可手动开 |
| **vehicle** | **可选开启** | **车端 mainboard 间高性能通信** |
| embedded | 关闭（资源紧张） | 可手动开 |
| mcu | ❌ 不支持 | 无 fork 系统调用 |

### 4.7 降级路径（关键安全设计）

**运行时自动检测**，任一条件不满足则降级到 FlatBuffers：

```cpp
ForkSHMMatcher {
  bool can_enable() {
    if (!can_disable_aslr()) return false;       // ASLR 无法关闭
    if (in_container_restricted()) return false; // 容器 seccomp 禁
    if (!daemon_running()) return false;          // daemon 未运行
    if (binary_hash_mismatch()) return false;     // 二进制 hash 不一致
    return true;
  }
};
```

降级路径：

```
ForkSHM mode（最快，vtable 直接解引用）
       ↓ 任一条件不满足
FlatBuffers 零拷贝（builder buffer memcpy，~10ns 额外开销）
       ↓ FlatBuffers 不适用（小消息）
POD memcpy（INTRA / SHM）
       ↓ 跨机
Zenoh / 序列化
```

### 4.8 多语言兼容（与 ADR-0007 协同）

| 语言 | ForkSHM mode 行为 |
|---|---|
| C++ | 完整支持 |
| Python / Rust / Go / Node | **透明降级到 FlatBuffers**（用户代码不变，性能略低） |

Python 用户写的 flow，如果消息是 `CrossProcessHandler` 类型，自动走 FlatBuffers 包装；C++ 用户写同样的 flow，自动走 ForkSHM mode。

### 4.9 安全缓解措施（针对 ASLR 关闭）

| 措施 | 说明 |
|---|---|
| 进程沙箱 | seccomp 限制系统调用，减少 ROP gadget 可用 |
| 内存标签 | ARM MTE / Intel LAM 标签内存 |
| 控制流完整性 | `-fcf-protection=full`（Intel CET） / `-mbranch-protection=standard`（ARM PAC/BTI） |
| 只读 SHM | SHM 区域对 worker 是只读（写需要特权） |
| daemon 隔离 | daemon 不接收外部输入，只响应 fork 请求 |

## 5. 工作量估算

| ID | 描述 | 估算 | Phase |
|---|---|---|---|
| L4-TRANS-13 | `tianshu-fork-daemon` 实现（生命周期 + ipc socket + hash 校验） | 6 | 2-3 |
| L4-TRANS-14 | ASLR 关闭 + binary layout hash 计算 | 3 | 2-3 |
| L4-TRANS-15 | SHM allocator（placement new + 跨进程分配） | 5 | 2-3 |
| L4-TRANS-16 | `TIANSHU_MARK_CROSS_PROCESS_SAFE` 静态检查宏 + 编译期 lint | 3 | 2-3 |
| L4-TRANS-17 | ForkSHM mode 降级路径 + FlatBuffers 自动切换 | 3 | 2-3 |
| L4-TRANS-18 | ForkSHM mode 安全缓解（seccomp + CFI + 只读 SHM） | 4 | 3 |
| L4-TRANS-19 | ForkSHM mode 集成测试 + 性能 benchmark | 4 | 3 |
| **合计** | - | **28 点** | **Phase 2-3** |

参考对比：

| 方案 | 工作量 | 性能 | 风险 |
|---|---|---|---|
| ForkSHM mode（本方案） | 28 点 | 最快（vtable 直解） | 高（ASLR + 单点） |
| offset_ptr（Boost.Interprocess 风格） | 8 点 | 接近最快（+1 加法） | 低 |
| FlatBuffers（已在 ADR-0008） | 已含 | 快（memcpy） | 无 |

## 6. 综合推荐

### 6.1 决策树

```
天枢主要场景是？
│
├── 车端 ECU（ORIN/J5），单机 mainboard 间通信，对延迟极敏感
│   └── 倾向 ForkSHM mode（接受工程复杂度）
│       └── 团队接受 ASLR 关闭 + 部署严苛？
│           ├── 是 → 实现完整 ForkSHM mode（28 点）
│           └── 否 → 用 offset_ptr（8 点，性能差距 < 5%）
│
├── 数据中心 / 云服务（容器化、多机）
│   └── 不用 ForkSHM（容器化困难，与跨机割裂）
│       └── 用 FlatBuffers（已在 ADR-0008）
│
├── 嵌入式 Linux（机器人、边缘 Box）
│   └── 用 offset_ptr（嵌入式不该关 ASLR）
│
└── MCU（Cortex-M）
    └── 无 SHM，无 fork；走 INTRA only
```

### 6.2 三选一推荐

| 选项 | 描述 | 工作量 | 推荐场景 |
|---|---|---|---|
| **A. 落地 ForkSHM mode** | 完整实现改良方案，作为 vehicle profile 可选模式 | 28 点 | 性能极致、车端专用、团队接受约束 |
| **B. 用 offset_ptr 替代** | Boost.Interprocess 风格，地址无关方案 | 8 点 | 主流做法，性能接近（< 5% 损失），无安全风险 |
| **C. 都不做** | 默认 FlatBuffers + POD memcpy 已经够好 | 0 点 | Phase 2 优先级低于其他模块 |

### 6.3 我的推荐：**B + Phase 3 探索 A**

**理由**：

1. **B 已能满足性能目标**（H2 假设：< 1% 差距），offset_ptr 加法开销可忽略
2. **A 的工程复杂度与安全风险**显著高于收益
3. **A 的多语言不友好**与 ADR-0007 多语言 SDK 冲突
4. **车端 ISO 21434** 强烈建议保留 ASLR
5. A 可作为 Phase 3 探索任务（如果实测发现 offset_ptr 不满足某极端场景）

### 6.4 如果用户坚持 A 的硬要求

那么建议：

- 仅作为 vehicle profile 的 **opt-in 模式**（默认关闭）
- **必须**配合 4.9 节安全缓解措施
- **必须**有自动降级到 FlatBuffers 的能力（4.7 节）
- 作为 ADR-0010 单独 ADR（不混入主架构）
- 在 ADR 中明确"**不推荐开源用户使用**，仅供内部高性能场景"

## 7. 与现有架构的整合方案（如果选 A）

### 7.1 在 [adr-0008 消息多格式](../adr/0008-message-format-multi.md) 中新增第 4 种格式

```cpp
// 第 4 种 MessageTraits 特化：跨进程 vtable 对象
template<CrossProcessSafe T>
struct MessageTraits<T> {
  static constexpr bool is_zero_copy = true;       // 真·零拷贝
  static constexpr bool requires_fork_shm = true;  // 仅 ForkSHM mode 下可用
  // ...
};
```

### 7.2 在 [adr-0007 多语言 SDK](../adr/0007-api-spec-multi-language.md) 中明确

| 语言 | ForkSHM mode | 替代 |
|---|---|---|
| C++ | ✅ 原生支持 | - |
| Python/Rust/Go/Node | ❌ 不支持 | 自动降级到 FlatBuffers |

### 7.3 在 [adr-0005 profile](../adr/0005-lightweight-multiplatform.md) 中标注

| Profile | ForkSHM mode |
|---|---|
| desktop | ⚠️ 可选（开发不强制） |
| server | ❌ 不推荐（容器化冲突） |
| vehicle | ⚠️ 可选（默认关闭，需显式开启） |
| embedded | ❌ 不推荐（ASLR 关闭风险） |
| mcu | ❌ 不支持 |

### 7.4 在 [02-开发计划表](../02-开发计划表.md) 中预留 L4-TRANS-13..19

工作量为 28 点，全部 Phase 2-3，**优先级 P2**（不阻塞关键路径）。

## 8. 待用户拍板的 fork

### Fork 1：主路线选择

| 选项 | 描述 | 工作量 |
|---|---|---|
| **A** | 落地 ForkSHM mode（改良方案） | 28 点 |
| **B** | 用 offset_ptr 替代（**推荐**） | 8 点 |
| **C** | 都不做，Phase 3 探索 | 0 点 |

### Fork 2（如果选 A）：默认开启 profile

- A1. vehicle profile 默认开启（性能优先）
- A2. 所有 profile 默认关闭，用户显式 opt-in（安全优先，**推荐**）

### Fork 3（如果选 A）：daemon 部署

- D1. systemd 系统服务（车端专用，启动最早）
- D2. 用户手动启动（开发友好）
- D3. mainboard 启动时按需起 daemon（自动化）

## 9. 决策后影响

如果用户选 B（推荐）：

| 影响项 | 变更 |
|---|---|
| `ALLOWED_DEPS.txt` | 加入 `boost:header-only:1.84+:offset_ptr-only`（[adr-0005 依赖治理](../adr/0005-lightweight-multiplatform.md) 禁 Boost 的例外） |
| 02-开发计划表 | 新增 L4-TRANS-13 offset_ptr 实现（8 点，Phase 2） |
| ADR-0008 | 新增第 4 种 MessageTraits：`OffsetPtrMessage<T>`（POD 子集 + offset_ptr） |
| ADR-0005 | Boost 解禁：`boost/interprocess/offset_ptr.hpp` 单 header 允许 |

如果用户选 A：

| 影响项 | 变更 |
|---|---|
| 02-开发计划表 | 新增 L4-TRANS-13..19（28 点，Phase 2-3） |
| ADR-0010 | 新建：ForkSHM mode 设计 |
| ADR-0005 | ASLR 关闭风险登记到风险表 |
| ADR-0008 | 新增第 4 种 MessageTraits：`CrossProcessSafeMessage<T>` |
| ADR-0007 | 多语言 SDK 明确自动降级 |

## 10. 附录：技术原理深挖

### 10.1 fork 后地址布局一致性证明

Linux fork(2) 内核实现（kernel/fork.c）：

1. `dup_mm()` 复制父进程的 `mm_struct`
2. `copy_page_range()` 复制页表项（VMA、prot、flags 完全一致）
3. 页面标记为 COW（写时复制）
4. ASLR 在 `randomize_page_tables()` / `randomize_stack_top()` 中应用，**只在 execve 时生效**，fork 不重新随机化
5. 因此 fork 出的子进程**完整继承**父进程地址布局

### 10.2 PIE 与 ASLR 的关系

| 编译选项 | 运行时 | 效果 |
|---|---|---|
| `-pie -fPIE`（默认） | ASLR on | 可执行文件基址随机化（每次 execve 变化） |
| `-pie -fPIE` | ASLR off | 可执行文件基址固定（但 fork 子进程同父） |
| `-no-pie -fno-PIE` | ASLR on/off | 可执行文件基址固定（编译时决定） |

**关键**：fork 的子进程是父的副本，无论 PIE 与否，子进程地址布局与父一致。

但**两个独立 execve 的进程**地址布局可能不同（即使代码相同）。

### 10.3 vtable 的内存布局

```cpp
class MyClass {
 public:
  virtual void foo();
  virtual void bar();
 private:
  int data_;
};

// 内存布局（64-bit Linux）：
// [vtable ptr] [data_]    <- 对象
//     ↓
//   [&MyClass::foo, &MyClass::bar]    <- vtable（在 .rodata 段）
```

vtable 本身在 `.rodata` 段，跟随 `.so`/`.exe` 加载基址。fork 子进程继承父进程 `.rodata` 映射，**vtable 地址完全一致**。

### 10.4 std::string 的内部布局（不可跨进程）

```cpp
std::string s = "hello";

// libstdc++ 实现（SSO + 堆分配）：
// - 短字符串（< 16 bytes）：内嵌在对象内存（可跨进程）
// - 长字符串：堆分配，对象内放堆指针（不可跨进程）
```

因此 `std::string` 短字符串可以跨进程（巧合），长字符串不行。但**不应该依赖此行为**（实现细节）。

### 10.5 ISO 21434 与 ASLR

ISO 21434（道路车辆网络安全工程）要求：

- 第 7 章"威胁分析"：ASLR 是关键缓解
- 第 11 章"运行安全"：ASLR 关闭属于"明确风险"

关闭 ASLR 在车端安全审计中**可能不合规**。但实际车端 ECU 多为封闭网络，ASLR 价值有限——这是工程权衡。

## 11. 参考资料

- Linux personality(2): https://man7.org/linux/man-pages/man2/personality.2.html
- ASLR: https://kernel.org/doc/Documentation/sysctl/kernel.txt (`randomize_va_space`)
- fork(2): https://man7.org/linux/man-pages/man2/fork.2.html
- Boost.Interprocess offset_ptr: https://www.boost.org/doc/libs/release/doc/html/interprocess/offset_ptr.html
- Iceoryx: https://iceoryx.io/
- CRIU: https://criu.org/
- ISO 21434: https://www.iso.org/standard/70918.html
- nginx master/worker: https://nginx.org/en/docs/control.html
- PostgreSQL shared buffers: https://www.postgresql.org/docs/current/runtime-config-resource.html
