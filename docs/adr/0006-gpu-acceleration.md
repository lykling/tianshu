# ADR-0006：GPU 加速与调度管理（设计就绪，实现按 profile 渐进）

- **状态**：设计已接受，**实现非关键路径**
- **日期**：2026-08-10
- **决策者**：Pride Leong
- **关联**：[adr/0005](./0005-lightweight-multiplatform.md) · [02-development-plan.md](../02-development-plan.md) · [00-overview.md](../00-overview.md)

## 实现策略（关键说明）

本 ADR 锁定 GPU 设计方向与接口契约，但 **GPU 实现不是 Phase 0 / Phase 1 的关键路径**。所有 GPU 相关功能点（L4-GPU-*、L3-GPU-*、L1-DSL-15..17、L1-CG-12、L2-LIN-8）的优先级 **P0 → P1/P2**，归属 Phase 2/3 按需推进。

这意味着：

| Phase | GPU 实现 |
|---|---|
| Phase 0 奠基 | ❌ 不实现 |
| Phase 1 PoC | ❌ 不实现（H1/H2/H3 验证用纯 CPU 链路） |
| Phase 2 MVP | ⚠️ **可选**（取决于选定的 perception mainboard 是否必须 GPU 推理；可先用 CPU 推理版跑通） |
| Phase 3 | ✅ 完整实现（含 MPS / MIG / OpenCL / NPU adapter / GPU SLA） |

**设计契约锁定的好处**：DSL 接口（`with_gpu_backend` / `with_gpu_memory` / `with_stream_policy`）、HAL 接口（`gpu.hpp`）、血缘字段（`gpu_device_id` 等）在 Phase 0 就定型，Phase 1 写 CPU 算子时不会"过度耦合 CPU"，未来加入 GPU 是"插入 backend"而不是"重构 API"。

---

## 背景

[ADR-0005](./0005-lightweight-multiplatform.md) 初版把 GPU 列为 `optional`，HAL-6 也定为 P2。这不符合实际需求：

- 自动驾驶的感知（camera / LiDAR 检测）、预测（轨迹推理）、规划（学习类）核心算子**强依赖 GPU**
- ORIN / J5 / MDC 等车端 ECU 的核心竞争力就是 GPU/NPU
- GPU OOM、显存碎片、热降频、推理超时是真实车端的**主要故障源**
- 跨 mainboard 多进程共享 GPU 是 Cyber RT 的痛点之一（无人协调）

天枢必须把 **GPU 加速 + GPU 调度管理**作为一类公民**设计**（即使早期实现按 profile 渐进），覆盖算子加速、资源管理、SLA 调度、容错降级全链路。

## 候选方案

### 方案 1：GPU 透明（让用户自己写 CUDA kernel）

天枢不管 GPU，用户在算子里直接调 CUDA / TensorRT。框架只在 transport 层支持 GPU pointer。

**优点**：实现简单；与 ROS 2 / Cyber 现状一致。
**缺点**：跨算子无法 GPU 内存复用；显存预算无人协调；GPU SLA 无法保证；OOM 直接 crash 整个 mainboard。

### 方案 2：GPU 完全托管（强制走天枢 GPU 抽象）

所有 GPU 算子必须走天枢的 GPU stream / memory pool，禁止裸 CUDA。

**优点**：完全可控；调度最优。
**缺点**：用户迁移成本高（要改全部 kernel 调用）；限制灵活性；自研负担重。

### 方案 3：分层 GPU 支持（**已选**）

- **底层**：天枢提供 GPU 资源管理（设备/stream/内存池/IPC 共享），用户**可选**使用
- **中层**：DSL 提供 GPU 算子声明（`with_gpu_backend<Backend::CUDA>`），编译器自动注入内存管理与 stream 绑定
- **上层**：SLA 调度把 GPU 视为 CPU 之外的二类资源（RTA 含 GPU 干扰），显存预算编译期校验

用户既可以用裸 CUDA（走底层 GPU 内存池），也可以用 DSL 声明（走中层自动管理），都可以获得 SLA 与容错保障。

## 决策

**选方案 3**：分层 GPU 支持，GPU 作为 SLA 调度的一类资源。

## GPU 技术栈选型

| 决策点 | 选定 | 理由 |
|---|---|---|
| **主 GPU API** | CUDA | ORIN/J5/Intel-AMD 主流车端 GPU 都是 CUDA 架构；生态最成熟 |
| **辅 GPU API** | OpenCL（嵌入式 GPU） + 国产 NPU SDK（如华为昇腾）通过 HAL backend | 兼容 non-CUDA 加速器 |
| **CPU-GPU 内存** | 默认 pinned memory + DMA（性能可控）；可选 unified memory（开发友好） | unified memory 有性能不稳问题，仅 desktop profile 默认 |
| **跨进程 GPU 共享** | 优先 CUDA MPS（多进程服务），MIG 作为硬隔离选项（ORIN 支持） | MPS 兼容性好；MIG 适合安全隔离场景 |
| **GPU stream 调度** | 编译期静态分配（每个算子绑定固定 stream）+ 运行时 fallback 到默认 stream | 静态分配避免运行时争用 |
| **推理引擎** | 不绑定（用户可选 TensorRT / ONNX Runtime / Triton），天枢提供通用 adapter | 保持灵活；TensorRT 是推荐默认 |

## 影响范围

### 新增框架

| 架构层 | 新框架 | 作用 |
|---|---|---|
| **L4** | F-L4-GPU | GPU 资源管理（设备/stream/内存池/IPC/事件/故障检测） |
| **L3** | F-L3-GPU | GPU SLA 调度（WCET/RTA/亲和/显存预算/降级） |
| **L1**（扩展现有） | L1-DSL 扩展 + L1-CG 扩展 | DSL 加 `with_gpu_backend` / codegen 注入 GPU 内存管理 |
| **L2**（扩展现有） | L2-LIN 扩展 | 血缘记录 GPU 张量引用（cudaIpcMemHandle） |
| **INFRA-HAL**（升级） | HAL-6 从 P2 → P1，vehicle profile 必做 | GPU 资源 backend（CUDA / OpenCL） |

### Profile 启用模块矩阵（修订 [adr/0005](./0005-lightweight-multiplatform.md)）

| 模块 | desktop | server | vehicle | embedded | mcu |
|---|---|---|---|---|---|
| **L4 GPU 资源管理（完整）** | ✅ | ✅ | ✅ | ❌ | ❌ |
| **L4 GPU 资源管理（lite，仅内存池）** | ✅ | ✅ | ✅ | ⚠️（仅 GPU SoC） | ❌ |
| **L3 GPU SLA 调度** | ✅ | ✅ | ✅ | ❌ | ❌ |
| **L1 DSL `with_gpu_backend`** | ✅ | ✅ | ✅ | ⚠️ | ❌ |

### 资源预算补充（修订 [adr/0005](./0005-lightweight-multiplatform.md)）

| Profile | GPU 显存预算 | GPU 利用率上限 |
|---|---|---|
| desktop | 不限 | 不限 |
| server | 不限 | 不限 |
| **vehicle** | < 8GB（ORIN 16GB 总显存留 50% 余量） | < 90%（留热降频余量） |
| embedded | < 2GB（如有 GPU） | < 80% |
| mcu | N/A | N/A |

## 设计要点

### L4-GPU-1 · GPU 设备抽象

```cpp
namespace tianshu::gpu {

enum class Backend { CUDA, OpenCL, NPU };  // HAL 决定具体 backend

class Device {
 public:
  static Device& get(int device_id = 0);
  Backend backend() const;
  int device_id() const;
  size_t total_memory() const;
  size_t free_memory() const;
  int compute_capability_major() const;  // CUDA sm_xx major
  // ...
};

}  // namespace tianshu::gpu
```

### L4-GPU-2 · GPU 内存池

- 默认 pinned memory + DMA（host-side pinned pool + device-side slab pool）
- 复用：相同 size class 走 free-list（避免 cudaMalloc/Free 频繁调用）
- 可观测：池容量、命中率、碎片率入 lineage
- IPC 共享：跨进程 GPU 内存通过 `cudaIpcGetMemHandle` / `cudaIpcOpenMemHandle`，handle 序列化到 SHM

### L4-GPU-3 · GPU Stream 管理

- 编译期：每个算子绑定固定 `cudaStream_t`（Pass 4 SLA 物理规划派生）
- 默认 stream 仅用于非关键路径（如启动期初始化）
- Stream 复用：相同 SLA 等级的算子可共享 stream（减少 context switch）

### L4-GPU-4 · CPU-GPU 数据零拷贝

- pinned memory → DMA → device memory（默认路径）
- unified memory（可选）：`cudaMallocManaged`，desktop profile 默认开
- IPC 共享：跨进程同一 GPU 张量通过 `cudaIpcMemHandle` 传递，零拷贝

### L4-GPU-5 · GPU 事件接入 DataNotifier

- `cudaEventRecord(stream)` → 完成时触发 DataNotifier callback
- 算子的输出发布依赖 GPU event，不是 CPU wall-clock
- 这让 SLA 调度器能看到真实 GPU 完成时间

### L4-GPU-6 · GPU 故障检测

- OOM：`cudaMalloc` 失败时触发 `gpu_oom` 事件
- Xid 错误：监听 `/dev/nvidia*/Xid*`（Linux）或 CUDA error callback
- 超时：stream 内 30s 无进展触发 `gpu_timeout`
- 容错决策器（L2-FT）订阅这些事件，按策略降级

### L3-GPU-1 · GPU WCET 估算

- 历史 P99.9 推理时间作为基准
- 按 batch size / 输入分辨率 / GPU 利用率插值
- 输出 worst-case GPU latency 给 RTA

### L3-GPU-2 · GPU 参与 RTA

- 任务在 CPU 和 GPU 上**串行占用**：CPU WCET + GPU WCET + 数据传输时间 = 总 WCET
- 多任务共享 GPU 时考虑 stream 内 FIFO 等待
- 输出"GPU 关键路径"与"CPU 关键路径"两条独立链路

### L3-GPU-3 · GPU 任务亲和

- 多 GPU 场景：把 SLA 最高的算子绑到 GPU0，其余轮询
- 单 GPU 场景：按 stream 隔离（不同 stream 不同算子）
- HAL 提供 GPU 拓扑（NVLink / PCIe 带宽），编译器利用

### L3-GPU-4 · 显存预算编译期校验

- 每个算子声明 `gpu_memory_bytes`（DSL `with_gpu_memory(bytes)`）
- 编译期 Pass 4 求和：所有同时驻留 GPU 的算子显存总和 ≤ profile 预算
- 超额加载期报错（不是运行时 OOM）：
  ```
  GPU MEMORY BUDGET EXCEEDED: total 12GB > budget 8GB
  Top contributors:
    [1] detect_op: 6GB (batch=8)
    [2] predict_op: 4GB
    [3] ...
  Suggestion: (a) 减小 batch_size (b) 启用梯度检查点 (c) 切到 DEGRADE fallback
  ```

### L3-GPU-5 · GPU OOM 自动降级

- 运行时 GPU OOM 事件 → L2-FT-4 DEGRADE 策略
- 自动切到 `flow_lite`（如 CPU 推理版本或低精度版本）
- 降级事件入 lineage，触发离线分析

## DSL 扩展

```cpp
void perception_flow(Node& node) {
  auto camera = node.reader<CameraMsg>("/perception/front");

  node.on_input({camera}, [&](auto c) {
    auto det = detect_op(c);  // 自动走 GPU（with_gpu_backend 声明）
    predict_out.write(predict_op(det));
  })
  .with_gpu_backend<Backend::CUDA>({              // 声明算子走 CUDA
    .device_id = 0,
    .stream_policy = StreamPolicy::DEDICATED,     // 独立 stream
    .memory_pool = "default_pinned",              // 用默认 pinned pool
  })
  .with_gpu_memory({                              // 显存预算声明
    .detect_op_bytes = 2_GB,
    .predict_op_bytes = 4_GB,
  })
  .with_sla(SLA{.deadline_ms = 50})
  .with_fallback("perception_flow_lite");         // OOM 自动切 CPU 版
}
```

## Codegen 扩展

Pass 5 codegen 时：

- 在算子函数前后注入 GPU stream 绑定 / 同步代码
- GPU 张量自动从内存池分配，发布时通过 IPC handle 传递（跨进程零拷贝）
- 显存预算编译期校验失败时，整个 mainboard 加载期 abort

## 血缘扩展

每条 GPU 张量消息的 lineage 含：

- `gpu_device_id`：在哪个 GPU 上分配
- `gpu_stream_id`：用哪个 stream
- `gpu_ipc_handle`：跨进程共享的 handle（如适用）
- `gpu_event_ts`：cudaEvent 完成时间戳（真实 GPU 完成时间）

## HAL 修订

`INFRA-HAL-6 GPU 资源 backend`：

- 优先级：P2 → **P1**（vehicle profile 必做）
- 估算：4 → **6 点**（含 MPS/MIG 集成）
- 实现范围：CUDA 5+ / OpenCL 1.2+ / 国产 NPU 适配层（按需）

## 资源预算补充

| 资源 | vehicle | embedded | 备注 |
|---|---|---|---|
| GPU 显存 | < 8GB | < 2GB | profile 阈值 |
| GPU 利用率 | < 90% | < 80% | 留热降频余量 |
| GPU stream 数 | < 16 | < 8 | 避免上下文切换过多 |

## 风险与缓解

| 风险 | 缓解 |
|---|---|
| CUDA 版本与车端 SDK 不一致 | 早期锁定 CUDA 12.x；HAL 抽象层隔离 |
| MPS 在车端 ECU 默认未启用 | 文档说明 + 自动启动 MPS daemon；MIG 作为 fallback |
| 国产 NPU 适配工作量大 | 第三方 SDK 通过 HAL adapter 接入；非主线 P0 |
| GPU OOM 检测延迟 | 主动监控 `cudaMemGetInfo`；预留 5% safety margin |
| GPU SLA 在热降频下不可满足 | RTA 输入含热状态分组 WCET；热触发时切 fallback |

## 后续可能演进

- 如果 AMD ROCm 在车端普及 → HAL 加 ROCm backend
- 如果华为昇腾 / 地平线 BPU 强需求 → HAL 加 NPU adapter（独立 ADR）
- 如果 GPU 任务抢占（CUDA 12+ preemption 试验特性）成熟 → 调度策略调整
- 如果 unified memory 性能稳定 → 默认切换到 unified（简化代码）

## 参考

- CUDA Programming Guide: https://docs.nvidia.com/cuda/cuda-c-programming-guide/
- CUDA MPS: https://docs.nvidia.com/deploy/mps/index.html
- CUDA MIG: https://docs.nvidia.com/datacenter/tesla/mig-user-guide/
- TensorRT: https://docs.nvidia.com/tensorrt/
