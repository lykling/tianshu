# ADR-0010：Transport 抽象 + 通用 SHM Allocator + INTRA（通用基础设施）

- **状态**：已接受
- **日期**：2026-08-10
- **决策者**：Pride Leong
- **关联**：[adr/0005](./0005-lightweight-multiplatform.md) · [adr/0007](./0007-api-spec-multi-language.md) · [adr/0008](./0008-message-format-multi.md) · [evaluation/0002](../evaluation/0002-fork-shared-address-space.md)

---

## 背景

天枢 L4-TRANS 框架目前规划了多个 transport 后端（INTRA / SHM / RTPS / Zenoh / ForkSHM / offset_ptr），存在以下问题：

1. **接口未统一**：用户难以在 backend 间切换；测试需要重写
2. **SHM allocator 各自为政**：多个模块（transport / lineage / GPU 内存池 / state checkpoint）都需要 SHM 分配，目前是每个模块自管，不通用，浪费内存，碎片化
3. **INTRA 模式未明确**：cyber 的 INTRA 模式（同进程内直接指针传递）是性能基础，需要在框架层明确
4. **offset_ptr 路线未落地**：[evaluation/0002](../evaluation/0002-fork-shared-address-space.md) 推荐 B 方案（offset_ptr）替代 ForkSHM mode，需要正式 ADR 锁定

本 ADR 把这 4 个紧密相关的"通用基础设施"统一设计。

## 决策

### 决策 1：Transport Backend 抽象层（接口统一）

定义统一接口，所有 transport 实现可插拔替换：

```cpp
namespace tianshu::transport {

enum class BackendType {
  INTRA,     // 同进程内直接指针传递
  SHM,       // 同机跨进程共享内存（offset_ptr 风格）
  RTPS,      // 跨机 RTPS（自研或第三方）
  ZENOH,     // 跨机 Zenoh（详见 evaluation/0001）
  CUSTOM,    // 用户自定义后端
};

struct ChannelConfig {
  std::string channel_name;
  std::string msg_type_name;  // 详见 adr/0008
  size_t queue_size = 16;
  QoS qos;                    // reliable / best_effort / ...
  MessageFormat format;       // POD / FlatBuffers / Protobuf
};

class TransportBackend {
 public:
  virtual ~TransportBackend() = default;

  // 工厂：创建 writer/reader 端
  virtual Result<WriterHandle> create_writer(const ChannelConfig& cfg) = 0;
  virtual Result<ReaderHandle> create_reader(const ChannelConfig& cfg) = 0;

  // 能力描述（编译期/运行期可查）
  virtual BackendType type() const = 0;
  virtual bool supports_zero_copy() const = 0;
  virtual bool supports_remote() const = 0;
  virtual bool supports_msg_format(MessageFormat f) const = 0;

  // 配置（profile 派生的 QoS、buffer size 等）
  virtual Status configure(const BackendConfig& cfg) = 0;

  // 服务发现挂钩
  virtual void on_peer_online(const PeerInfo& peer) = 0;
  virtual void on_peer_offline(const PeerInfo& peer) = 0;
};

// Registry：用户/编译器注册自定义后端
class TransportRegistry {
 public:
  static TransportRegistry& instance();
  void register_backend(BackendType type, BackendFactory factory);
  TransportBackend* get(BackendType type);
};

}  // namespace tianshu::transport
```

### HYBRID 自动选择（默认）

```cpp
class HybridTransport : public TransportBackend {
  // 同进程 → INTRA（自动检测 process_id）
  // 同机 → SHM（offset_ptr）
  // 跨机 → Zenoh（详见 evaluation/0001）
  //
  // 选择发生在 channel 创建时，由 service discovery 提供信息
};
```

**编译器集成**：Pass 4 SLA 物理规划时，编译器根据 channel 配置 + peer 分布自动选择 backend，**用户无感知**。

### 用户接口（DSL 透明）

用户写 flow 时**不需要指定 backend**：

```cpp
node.on_input({camera, lidar}, [&](auto c, auto l) {
  // 编译器决定 camera 走 INTRA、lidar 走 SHM、跨机 channel 走 Zenoh
});
```

高级用户可显式指定：

```cpp
node.reader<CameraMsg>("/perception/front")
    .with_backend(BackendType::SHM)        // 强制 SHM
    .with_fallback_backend(BackendType::ZENOH);  // SHM 不可用则降级
```

### 决策 2：通用 SHM Allocator（框架公共能力）

提供统一的 SHM 分配器，所有需要 SHM 的模块共享：

```cpp
namespace tianshu::shm {

class ShmPool {
 public:
  static ShmPool& instance();

  // 大块分配（slab pool + size class）
  void* allocate(size_t bytes, size_t alignment = alignof(std::max_align_t));
  void deallocate(void* p);

  // 固定大小对象池（无锁 free-list）
  template<typename T>
  ObjectPool<T>& get_object_pool();

  // STL allocator 适配（用于 std::vector<T, ShmAllocator<T>> 等）
  template<typename T>
  Allocator<T> get_allocator();

  // offset_ptr 友好（返回指针可用于 offset_ptr 引用）
  // 大消息池（> 1MB）单独 slab
  void* allocate_large(size_t bytes);
  void deallocate_large(void* p);

  // 可观测性
  struct Stats {
    size_t total_bytes;
    size_t used_bytes;
    size_t free_bytes;
    double fragmentation_ratio;
    double hit_rate;  // 命中已分配 chunk 而非新分配
  };
  Stats stats() const;
};

}  // namespace tianshu::shm
```

### 多池策略

| Size Class | 范围 | 策略 | 用途 |
|---|---|---|---|
| Small | 8B - 256B | per-thread cache + central free-list | 小消息、控制类、状态元数据 |
| Medium | 256B - 64KB | slab pool + size class 桶 | 中等消息（点云帧、相机帧） |
| Large | 64KB - 1MB | 单独 slab + 簇分配 | 大消息（高分辨率图像、LiDAR scan） |
| Huge | > 1MB | mmap + 显式释放 | 超大消息（批数据、模型权重） |

### Profile 感知

| Profile | 总池容量 | 多池策略 |
|---|---|---|
| desktop | 不限 | 全套（4 池） |
| server | 不限 | 全套（4 池） |
| vehicle | 256MB 默认 | 全套（4 池），但 huge 池上限 100MB |
| embedded | 32MB | 简化（3 池，无 huge） |
| mcu | 256KB | 极简（2 池：small + medium，无 large/huge） |

### 谁用这个 allocator？

| 模块 | 用途 |
|---|---|
| L4-TRANS SHM backend | 消息 buffer |
| L4-TRANS offset_ptr | SHM 内对象 |
| L2-LIN lineage | 历史帧 ring buffer |
| L4-GPU 内存池 | pinned memory host side |
| L1-DSL with_state | 状态 checkpoint |
| L2-RB ring buffer | 容错重放 |
| 用户代码（公开 API） | 用户的 SHM 共享对象 |

### offset_ptr 集成

[evaluation/0002](../evaluation/0002-fork-shared-address-space.md) 推荐 B 方案落地：

```cpp
namespace tianshu::shm {

// offset_ptr：跨进程安全指针
template<typename T>
class offset_ptr {
 public:
  offset_ptr() : offset_(0) {}
  offset_ptr(T* p) : offset_(reinterpret_cast<char*>(p) - reinterpret_cast<char*>(this)) {}

  T* get() const { return reinterpret_cast<T*>(reinterpret_cast<char*>(const_cast<offset_ptr*>(this)) + offset_); }
  T* operator->() const { return get(); }
  T& operator*() const { return *get(); }
  explicit operator bool() const { return offset_ != 0; }

 private:
  intptr_t offset_;  // 相对偏移
};

// SHM 友好的容器
template<typename T>
using vector = std::vector<T, Allocator<T>>;

using string = std::basic_string<char, std::char_traits<char>, Allocator<char>>;

}  // namespace tianshu::shm
```

**自研 vs Boost**：

- Boost.Interprocess 的 `offset_ptr` 是 header-only 单文件（~500 行）
- [adr-0005 依赖治理](./0005-lightweight-multiplatform.md) 禁 Boost 全套
- **决策**：自研 `tianshu::shm::offset_ptr`（~100 行精简版），零第三方依赖
- 工作量：3 点（参考 Boost 实现）

### 决策 3：INTRA 模式（同进程内直接指针传递）

完全对标 cyber 的 INTRA，是默认启用的高性能后端：

```cpp
namespace tianshu::transport::intra {

class IntraBackend : public TransportBackend {
 public:
  BackendType type() const override { return BackendType::INTRA; }
  bool supports_zero_copy() const override { return true; }  // 真·零拷贝
  bool supports_remote() const override { return false; }    // 不跨进程
  bool supports_msg_format(MessageFormat f) const override { return true; }  // 全格式

  // 实现：writer 端直接调所有 reader 的 callback，零拷贝零序列化
  Result<WriterHandle> create_writer(const ChannelConfig& cfg) override;
  Result<ReaderHandle> create_reader(const ChannelConfig& cfg) override;
};

}  // namespace tianshu::transport::intra
```

**性能**：
- 延迟：~10-100ns/消息（直接函数调用）
- 无序列化（消息对象直接传指针）
- 无 SHM 分配（消息在 writer 进程内）
- 无锁（同进程内）

**自动启用**：service discovery 检测到 reader 和 writer 在同进程时，HYBRID 自动切换到 INTRA。

### 决策 4：与 ADR-0007 多语言 SDK 协同

C ABI 边界（[adr/0007](./0007-api-spec-multi-language.md)）暴露的 transport 接口：

```c
// ffi/transport_c.h
typedef struct tianshu_transport_t* tianshu_transport_handle;

tianshu_status_t tianshu_transport_create(
    tianshu_transport_handle* out,
    int backend_type);  // 0=HYBRID, 1=INTRA, 2=SHM, 3=ZENOH, ...

tianshu_status_t tianshu_transport_create_writer(
    tianshu_transport_handle tp,
    const char* channel,
    const char* msg_type,
    /* ... */
    int* out_writer_id);
```

多语言 SDK 通过 C ABI 调用，**自动获得 backend 切换能力**。

## 决策依据

### 为什么需要统一接口

| 问题 | 统一接口解决 |
|---|---|
| 用户测试时换 backend 难 | 一行配置切换 |
| CI 矩阵爆炸（每 backend 跑全套） | 抽象层 + 数据驱动测试 |
| 引入新 backend（如未来 RDMA、QUIC）实现接口即可 | 扩展性 |
| 跨机方案评估（evaluation/0001）切换不影响用户代码 | 透明 |

### 为什么需要通用 SHM allocator

| 问题 | 通用 allocator 解决 |
|---|---|
| 各模块自管 SHM 浪费内存 | 共享池，按需分配 |
| 内存碎片化 | 统一的 size class 策略 |
| 调试困难（不知道谁用了 SHM） | 统一可观测性 |
| 用户想用 SHM 写自定义对象 | 公开 API 可用 |
| offset_ptr 集成复杂 | allocator 内建支持 |

### 为什么 INTRA 是默认

| 维度 | INTRA | SHM | 跨机 |
|---|---|---|---|
| 延迟 | 10-100ns | 1-10μs | 0.5-5ms |
| 内存开销 | 无 | SHM buffer | 网络 buffer |
| 序列化 | 无 | 仅跨进程格式 | 必须 |
| 适用 | 同进程多 component | 多 mainboard | 多机 |

天枢大部分 hot path 是同进程内（一个 mainboard 内多个 component），INTRA 应该是默认。

## 影响范围

### 新增 L4-TRANS 框架扩展

| ID | 描述 | 优先级 | 估算 | Phase |
|---|---|---|---|---|
| L4-TRANS-18 | `TransportBackend` 抽象接口 + Registry | P0 | 3 | 1 |
| L4-TRANS-19 | `HybridTransport` 自动选择策略 | P0 | 3 | 1 |
| L4-TRANS-20 | `IntraBackend` 完整实现（zero-copy，所有消息格式支持） | P0 | 3 | 1 |
| L4-TRANS-21 | INTRA 自动检测（service discovery 集成） | P0 | 2 | 1 |
| L4-TRANS-22 | `tianshu::shm::ShmPool` 通用 allocator（4 池策略 + Stats） | P0 | 5 | 1 |
| L4-TRANS-23 | `tianshu::shm::Allocator<T>` STL 适配 | P0 | 2 | 1 |
| L4-TRANS-24 | `tianshu::shm::offset_ptr<T>` 自研（参考 Boost 精简版） | P0 | 3 | 1 |
| L4-TRANS-25 | `tianshu::shm::vector<T>` / `string` SHM 容器 | P1 | 2 | 2 |
| L4-TRANS-26 | SHM allocator 可观测性（容量/命中率/碎片率） | P1 | 2 | 2 |
| L4-TRANS-27 | SHM allocator profile 配置（vehicle/embedded/mcu 阈值） | P1 | 1 | 2 |

### 用户代码改造

旧的 L4-TRANS-2（INTRA backend）/ L4-TRANS-3（SHM backend）/ L4-TRANS-13（offset_ptr）等，都**改为实现 `TransportBackend` 接口**，工作内容不变但接口统一。

### 其他模块受益

| 模块 | 改造 |
|---|---|
| L4-GPU-2 GPU 内存池 | host-side pinned 改用 ShmPool（device-side 仍独立） |
| L2-LIN-3 LineageBuffer | 用 ShmPool 代替自管 ring buffer |
| L2-RB-1 RingBuffer | 直接用 ShmPool |
| L1-DSL-9 with_state | state checkpoint 用 ShmPool |

### 与 evaluation/0002 的关系

- evaluation/0002 推荐 B 方案（offset_ptr）→ **本 ADR 落地**
- ForkSHM mode（A 方案）仍保留为 P2 占位（L4-TRANS-14..17），未来有强需求时再激活

### 工作量影响

- L4-TRANS 新增 10 个功能点（L4-TRANS-18..27），共 ~26 点
- 大部分 P0（Phase 1 必做基础）
- 全局 P0 +20 点（26 中的 P0 部分）

## Profile 启用矩阵（修订）

| 模块 | desktop | server | vehicle | embedded | mcu |
|---|---|---|---|---|---|
| TransportBackend 抽象 | ✅ | ✅ | ✅ | ✅ | ✅ |
| HYBRID 自动选择 | ✅ | ✅ | ✅ | ✅ | ⚠️（INTRA only） |
| INTRA backend | ✅ | ✅ | ✅ | ✅ | ✅ |
| SHM backend（offset_ptr） | ✅ | ✅ | ✅ | ✅ | ⚠️（stream buffer） |
| RTPS backend | ✅ | ✅ | ⚠️ | ❌ | ❌ |
| Zenoh backend | ✅ | ✅ | ⚠️ | ❌ | ❌ |
| ForkSHM mode | ⚠️ opt-in | ❌ | ⚠️ opt-in | ❌ | ❌ |
| ShmPool（4 池） | ✅ | ✅ | ✅ | ⚠️（3 池） | ⚠️（2 池） |
| offset_ptr | ✅ | ✅ | ✅ | ✅ | ✅ |
| SHM 容器（vector/string） | ✅ | ✅ | ✅ | ✅ | ⚠️ |

## 风险与缓解

| 风险 | 缓解 |
|---|---|
| TransportBackend 接口设计过早抽象 → 未来扩展受限 | Phase 0 多花时间评审；保留 `CUSTOM` backend 类型 |
| ShmPool 全局单例 → 多 mainboard 进程冲突 | 每 mainboard 独立 ShmPool 实例（按 process_id 隔离） |
| offset_ptr 自研 bug → 跨进程崩溃 | 充分单测 + sanitizer；参考 Boost 成熟实现 |
| HYBRID 选择逻辑出错 → 性能退化 | 编译期可指定 backend；运行时可强制覆盖 |
| 各模块从自管 allocator 迁移到 ShmPool 工作量大 | 分模块渐进迁移（Phase 1 完成 transport，Phase 2 lineage + state） |

## 后续可能演进

- 如果未来引入 RDMA / GPU direct → 实现 `RdmaBackend` + `GpuDirectBackend`
- 如果未来支持 QUIC → 实现 `QuicBackend`
- 如果未来 SHM allocator 需求复杂 → 引入更先进的 allocator（如 mimalloc / jemalloc SHM 版本）
- 如果未来 offset_ptr 自研不够用 → 退回 Boost.Interprocess（已评估可单 header 提取）

## 附录：基础类 SHM 兼容策略

### 原则：统一在分配器层解决，不在每个类上加 SHM 变体

ObjectPool / CacheBuffer / AtomicHashMap 等基础类**不感知 SHM**。
SHM 兼容通过以下机制实现：

1. **Index-based free-list（ObjectPool 已采用）**：内部用 index 而非指针管理 free-list，
   天然跨进程安全。不需要 `offset_ptr`。

2. **外部内存构造**：基础类提供"接收外部内存"的构造函数，由 ShmPool 分配后传入：

```cpp
// 当前（堆分配）
ObjectPool<T> pool(1024);

// 未来（SHM 分配，同一套代码）
void* mem = shm_pool.allocate(1024 * sizeof(ObjectPool<T>::Slot));
ObjectPool<T> pool(mem, 1024);
```

3. **`std::atomic` 跨进程安全**：x86/ARM 硬件保证 cache-coherent atomic 跨进程可见。
   C++ 标准标记为 implementation-defined，但 GCC/Clang 实现依赖硬件保证。
   实践中安全（Phase 2 加运行期检测注释）。

### 各基础类的 SHM 兼容方式

| 类 | 需要改什么 | SHM 变体？ | 原因 |
|---|---|---|---|
| ObjectPool<T> | 加 1 个外部内存构造函数 | ❌ 不需要 | Index-based free-list 已跨进程安全 |
| CacheBuffer<T> | 加 1 个外部内存构造函数 | ❌ 不需要 | 环形 buffer 用 index 头尾指针 |
| AtomicHashMap<K,V> | 加 1 个外部内存构造函数 | ❌ 不需要 | 开放寻址 + atomic CAS，index-based |
| AtomicRWLock | 不需要改 | ❌ 不需要 | 基于 atomic flag，无指针 |
| SpinLock / TicketLock | 不需要改 | ❌ 不需要 | 纯 atomic 整数 |
| BlockingCounter | 不需要改 | ❌ 不需要 | atomic + futex/eventfd |
| ShmPool | - | ✅ 它本身就是 SHM 层 | 4 池策略 + offset_ptr 支持 |

### Phase 1 落地

Phase 1 写基础类时**只写堆版本**（`new[]` 内部分配），SHM 兼容构造函数在 Phase 2
（ShmPool 实现后）统一加。这样 Phase 1 不引入 SHM 依赖，保持简单。

## 参考

- Boost.Interprocess: https://www.boost.org/doc/libs/release/doc/html/interprocess.html
- Iceoryx: https://iceoryx.io/
- Apex.OS SHM allocator 设计: https://github.com/apexai/apex_os
- tcmalloc / jemalloc 设计（size class 策略）
