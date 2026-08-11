# ADR-0007：接口规范与多语言支持

- **状态**：设计已接受，**C++ + C ABI 是 P0，多语言 SDK 实现 Phase 2/3 按需**
- **日期**：2026-08-10
- **决策者**：Pride Leong
- **关联**：[adr/0001](./0001-dsl-form.md) · [adr/0005](./0005-lightweight-multiplatform.md) · [adr/0006](./0006-gpu-acceleration.md) · [02-development-plan.md](../02-development-plan.md)

---

## 背景

天枢核心实现语言是 C++（DSL、trace、codegen 都基于 C++20）。但实际场景中：

- **Python**：算法团队、研究、笔记本、可视化
- **Rust**：安全敏感场景（车端控制）、高性能工具
- **Go**：云服务、运维工具、监控
- **Node.js / TypeScript**：Web 监控面板、可视化、工具链

如果 C++ API 设计**不预留多语言入口**，未来加多语言会触发大规模重构：

- C++ 模板/异常/RTTI 跨不过 FFI 边界
- STL 容器不能直接给其他语言
- 头文件耦合导致 ABI 不稳定
- 命名规范不一致让绑定代码丑陋

本 ADR 从 Phase 0 起锁定接口规范，让多语言支持在 Phase 2/3 渐进加入时**零重构成本**。

## 候选方案

### 方案 1：C++ Only

不预留多语言入口，未来按需大改。

**优点**：早期简单。
**缺点**：未来重写；研究/工具生态被排除。

### 方案 2：C ABI 通用接口（最低公分母）

所有功能暴露为 C ABI 函数，其他语言用 FFI 直调。

**优点**：通用，所有语言都能 link。
**缺点**：用户体验差（C API 不友好）；每语言都要写胶水；无类型安全。

### 方案 3：分层多语言 SDK（**已选**）

三层结构：

1. **C++ 核心**：高性能实现，C++20 模板化
2. **C ABI 边界**：稳定的 extern "C" 接口，作为 FFI 锚点（不暴露 C++ 类型）
3. **各语言原生 SDK**：基于 C ABI 包装出符合语言习惯的 API（pybind11 / cxx / cgo / napi）

Phase 0/1 只做第 1、2 层；Phase 2/3 按需做第 3 层。

## 决策

**选方案 3**：C++ 核心 + C ABI 边界 + 多语言 SDK 渐进。

## API 规范总则

### 1. 兼容性策略（SemVer）

- 版本号 `MAJOR.MINOR.PATCH`
- MAJOR：ABI/源码不兼容变更
- MINOR：向后兼容新增
- PATCH：bug 修复
- **Phase 0 起 v0.x**（实验期允许破坏），**Phase 2 起 v1.x**（API 冻结）
- 每个 release 注明 ABI 兼容性等级

### 2. 命名空间

```cpp
namespace tianshu::v1 {            // 顶层命名空间含版本号
  namespace core {}                // Node/Reader/Writer/Component
  namespace dsl {}                 // DSL fluent builder
  namespace trace {}
  namespace compiler {}
  namespace lineage {}
  namespace sla {}
  namespace gpu {}                 // GPU（详见 adr/0006）
  namespace osal {}                // OS 抽象（详见 adr/0005）
  namespace hal {}                 // 硬件抽象（详见 adr-0005）
  namespace ffi {}                 // C ABI（extern "C"）
}
```

### 3. 命名规范

| 类别 | 规范 | 示例 |
|---|---|---|
| 类型/类 | PascalCase | `Node`, `Reader`, `Component` |
| 函数/方法 | snake_case | `create_reader`, `try_fetch` |
| 成员变量 | snake_case_ | `deadline_ms_`, `priority_` |
| 常量/宏 | UPPER_SNAKE_CASE | `TIANSHU_PROFILE_VEHICLE` |
| 模板参数 | PascalCase + T 前缀 | `TMsg`, `TBackend` |
| Concept | PascalCase + Concept 后缀 | `MessageConcept`, `FlowBuilderConcept` |
| C ABI 函数 | `tianshu_<module>_<verb>_<noun>` | `tianshu_core_create_reader` |

### 4. 错误处理

| 边界 | 策略 |
|---|---|
| C++ 内部 | `std::expected<T, ErrorCode>`（C++23）/ `tl::expected`（C++20 过渡） |
| C ABI 边界 | 整数错误码 + out-parameter，**绝不抛 C++ 异常出 FFI** |
| Python/Node/Rust SDK | 转换为语言原生错误（Python Exception / Result<T,E> / Promise reject） |

错误码枚举：

```c
typedef enum tianshu_status_t {
  TIANSHU_OK = 0,
  TIANSHU_ERROR_INVALID_ARGUMENT = 1,
  TIANSHU_ERROR_CHANNEL_NOT_FOUND = 2,
  TIANSHU_ERROR_SLA_VIOLATION = 3,
  TIANSHU_ERROR_TRACE_FAILED = 4,
  TIANSHU_ERROR_CODEGEN_FAILED = 5,
  TIANSHU_ERROR_GPU_OOM = 6,
  TIANSHU_ERROR_OUT_OF_MEMORY = 100,
  TIANSHU_ERROR_INTERNAL = 255,
} tianshu_status_t;
```

### 5. 内存所有权

- **谁分配谁释放**原则：C ABI 提供配对的 create/destroy
- 不透明指针模式：`typedef struct tianshu_node_t* tianshu_node_handle;`
- C++ → 其他语言：用 unique_ptr / shared_ptr 包装 handle
- 其他语言 → C++：通过 ABI 传入 primitive（id, handle, opaque ptr）

### 6. 头文件组织

```
tianshu/
├── include/tianshu/             # 公开头文件（用户可见）
│   ├── tianshu.h                # 总入口（aggregate header）
│   ├── core/
│   │   ├── node.h
│   │   ├── reader.h
│   │   └── writer.h
│   ├── dsl.h
│   ├── trace.h
│   └── ffi/                     # C ABI 头文件（无 C++ 依赖）
│       ├── core_c.h
│       ├── dsl_c.h
│       └── trace_c.h
└── src/                         # 私有实现（用户不可见）
```

公开头文件禁止 `#include` 私有头文件，反向也不允许。

### 7. 二进制布局

- 公开类型**不用** STL 容器（用自定义 span / array_view）
- 公开类型 POD-friendly（可跨语言 memcpy）
- 字节序：默认 little-endian（与主流硬件一致）
- 对齐：8 字节对齐（C ABI 跨语言安全）

## C ABI 设计

### 不透明指针模式

```c
// ffi/core_c.h

typedef struct tianshu_node_t* tianshu_node_handle;
typedef struct tianshu_reader_t* tianshu_reader_handle;
typedef struct tianshu_writer_t* tianshu_writer_handle;
typedef struct tianshu_msg_t* tianshu_msg_handle;

typedef struct tianshu_init_options_t {
  int argc;
  const char* const* argv;
  const char* config_path;     // 可选
} tianshu_init_options_t;

// 生命周期：create → use → destroy
tianshu_status_t tianshu_init(const tianshu_init_options_t* opts);
void tianshu_shutdown(void);

tianshu_status_t tianshu_core_create_node(tianshu_node_handle* out);
void tianshu_core_destroy_node(tianshu_node_handle node);

tianshu_status_t tianshu_core_create_reader(
    tianshu_node_handle node,
    const char* channel,
    const char* msg_type_name,    // 类型名（避免 C 模板）
    size_t queue_size,
    tianshu_reader_handle* out);

tianshu_status_t tianshu_core_reader_try_fetch(
    tianshu_reader_handle reader,
    tianshu_msg_handle* out_msg,
    int64_t* out_ts_ns);

tianshu_status_t tianshu_core_writer_write(
    tianshu_writer_handle writer,
    const void* payload,
    size_t payload_size,
    const char* msg_type_name);
```

### 消息跨语言契约

- 消息 payload 在 C ABI 边界**序列化为字节数组**（Protobuf lite）
- 类型识别用字符串名（`"tianshu.proto.CameraMsg"`），不用 C++ RTTI
- 大消息（>64KB）走 SHM，handle 而非 bytes

### 错误描述获取

```c
const char* tianshu_status_str(tianshu_status_t status);
const char* tianshu_last_error(void);  // thread-local 错误描述
```

## 多语言 SDK

### 设计原则

- **零拷贝**：消息在大消息场景下零拷贝（SHM handle 传递，不序列化）
- **语言原生**：API 符合语言习惯（Python 用 context manager，Rust 用 Result，Go 用 context/error）
- **类型安全**：消息类型在编译期校验（Python 用 typing，Rust 用 trait，Go 用 generics）
- **异步友好**：Python asyncio / Rust tokio / Go goroutine / Node Promise

### 各语言策略

| 语言 | 绑定技术 | 优先级 | Phase | 备注 |
|---|---|---|---|---|
| **Python** | pybind11（成熟稳定） | P1 | 2 | 算法团队首要需求 |
| **Rust** | cxx + cbindgen + bindgen | P2 | 2-3 | 安全敏感场景；cxx 比 raw FFI 安全 |
| **Go** | cgo + 手写绑定 | P2 | 3 | 云服务场景；cgo 有性能成本 |
| **Node.js** | napi-rs（Rust 写绑定） | P2 | 3 | Web 工具链 |
| **C#** | P/Invoke（直接调 C ABI） | P3 | 不规划 | 仅在有强需求时考虑 |
| **Java/Kotlin** | JNI / Project Panama | P3 | 不规划 | 同上 |

### Python SDK 示例（设计契约）

```python
import tianshu
from tianshu.dsl import Node, flow
from tianshu.protos import CameraMsg, LidarCloud

tianshu.init()

@flow("perception_flow")
def perception_flow(node: Node):
    camera = node.reader("/perception/front", CameraMsg)
    lidar = node.reader("/lidar/points", LidarCloud)

    @node.on_input([camera, lidar])
    @node.with_sla(deadline_ms=50)
    @node.with_fallback("perception_flow_lite")
    def _(c: CameraMsg, l: LidarCloud):
        det = detect_op(c, l)
        predict_out.write(predict_op(det))

# mainboard 启动时自动 trace + 编译
```

### Rust SDK 示例（设计契约）

```rust
use tianshu::{Node, Reader, dsl::flow};
use tianshu::protos::{CameraMsg, LidarCloud};

#[flow("perception_flow")]
fn perception_flow(node: &Node) {
    let camera = node.reader::<CameraMsg>("/perception/front");
    let lidar = node.reader::<LidarCloud>("/lidar/points");

    node.on_input((&camera, &lidar))
        .with_sla(Sla { deadline_ms: 50 })
        .with_fallback("perception_flow_lite")
        .run(|c: CameraMsg, l: LidarCloud| {
            let det = detect_op(&c, &l);
            predict_out.write(predict_op(&det));
        });
}
```

### Go SDK 示例（设计契约）

```go
package main

import (
    "github.com/lykling/tianshu-go"
    "github.com/lykling/tianshu-go/dsl"
)

func main() {
    tianshu.Init()
    defer tianshu.Shutdown()

    dsl.Flow("perception_flow", func(node *tianshu.Node) {
        camera := node.Reader("/perception/front", &CameraMsg{})
        lidar := node.Reader("/lidar/points", &LidarCloud{})

        node.OnInput(camera, lidar).
            WithSLA(tianshu.SLA{DeadlineMs: 50}).
            WithFallback("perception_flow_lite").
            Run(func(c *CameraMsg, l *LidarCloud) {
                det := detectOp(c, l)
                predictOut.Write(predictOp(det))
            })
    })
}
```

### Node.js / TypeScript SDK 示例（设计契约）

```typescript
import { Node, dsl } from '@tianshu/node';
import { CameraMsg, LidarCloud } from '@tianshu/proto';

tianshu.init();

dsl.flow('perception_flow', (node: Node) => {
  const camera = node.reader<CameraMsg>('/perception/front');
  const lidar = node.reader<LidarCloud>('/lidar/points');

  node.onInput(camera, lidar)
    .withSla({ deadlineMs: 50 })
    .withFallback('perception_flow_lite')
    .run((c, l) => {
      const det = detectOp(c, l);
      predictOut.write(predictOp(det));
    });
});
```

## 跨语言契约

### 序列化

- **必须**：Protobuf（含 lite runtime，跨语言好）— 详见后续 ADR-0011 序列化选型
- **可选**：FlatBuffers（零拷贝场景）、Cap'n Proto（同类）
- JSON 仅用于调试和 Web，不用于核心消息

### 消息类型注册

```cpp
// C++ 端
TIANSHU_REGISTER_MESSAGE(CameraMsg, "tianshu.proto.CameraMsg");
```

```python
# Python 端（自动从 .proto 生成）
from tianshu.protos import CameraMsg  # 自动注册
```

### 跨语言回调

- C++ ↔ Python：pybind11 GIL 管理
- C++ ↔ Rust：cxx safety boundary，禁止 C++ 异常进 Rust
- C++ ↔ Go：cgo 必须释放 Goroutine 绑定的 OS 线程
- C++ ↔ Node：libuv event loop 集成，callback 切到主线程

### Profile 与多语言

| Profile | 多语言支持 |
|---|---|
| desktop | ✅ Python/Rust/Go/Node 全支持 |
| server | ✅ 同 desktop |
| vehicle | ⚠️ Python + Rust（其他可选） |
| embedded | ⚠️ Rust only（资源约束） |
| mcu | ❌ 仅 C ABI（其他语言 runtime 装不下） |

## 影响范围

### 新增 INFRA 框架

| 框架 | 作用 |
|---|---|
| F-INFRA-API | API 规范、C ABI、多语言绑定 |

### INFRA-API 功能点概要

- API 规范文档（命名/ABI/兼容性）
- C ABI 设计 + 头文件
- 头文件组织（public/private 分离）
- API lint（CI 守护：命名规范、ABI 兼容性）
- Python SDK（pybind11）
- Rust SDK（cxx）
- Go SDK（cgo）
- Node.js SDK（napi-rs）

详见 [02-development-plan F-INFRA-API](../02-development-plan.md#f-infra-api--api-规范与多语言绑定)。

### 与其他 ADR 的协同

- [ADR-0001 DSL 选型](./0001-dsl-form.md)：DSL fluent builder 风格天然适配多语言（每语言都能写 fluent builder）
- [ADR-0005 轻架构](./0005-lightweight-multiplatform.md)：依赖治理需考虑多语言绑定库限制（如 PyO3 不能在嵌入式跑、cgo 有性能成本）
- [ADR-0006 GPU](./0006-gpu-acceleration.md)：GPU 类型在 C ABI 边界用 opaque handle，不暴露 CUDA 类型

## 风险与缓解

| 风险 | 缓解 |
|---|---|
| C ABI 设计错误导致大重构 | Phase 0 多花时间评审 ABI；保留 ≥ 2 位资深 reviewer |
| 多语言 SDK 维护成本爆炸 | Phase 2/3 严格按需；不主动实现无强需求的语言 |
| 序列化格式与多语言绑定耦合 | 序列化 ADR-0011 单独评审 |
| Python GIL 影响实时性 | Python 仅用于算法开发/调试，不进 vehicle 关键路径 |
| Rust cxx 限制 | 早期确认 cxx 能力边界，复杂场景走 raw FFI |

## 后续可能演进

- 如果某语言生态强烈需求（如 Lua 嵌入式脚本） → 走 C ABI + 该语言绑定
- 如果 Protobuf 性能瓶颈明显 → 切 FlatBuffers（C ABI 不变，序列化层换）
- 如果 WebAssembly 强需求 → Emscripten 编译 C++ + 自动 JS 绑定
- 如果未来需要分布式跨语言 → 在 SDK 上层加 gRPC / Thrift 服务化

## 参考

- pybind11: https://pybind11.readthedocs.io/
- cxx: https://cxx.rs/
- cbindgen: https://github.com/mozilla/cbindgen
- bindgen: https://rust-lang.github.io/rust-bindgen/
- napi-rs: https://napi.rs/
- SemVer: https://semver.org/
