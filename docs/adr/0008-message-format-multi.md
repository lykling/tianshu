# ADR-0008：消息格式多支持（FlatBuffers / Protobuf / 自定义 struct）

- **状态**：已接受
- **日期**：2026-08-10
- **决策者**：Pride Leong
- **关联**：[adr/0001](./0001-dsl-form.md) · [adr/0005](./0005-lightweight-multiplatform.md) · [adr/0007](./0007-api-spec-multi-language.md) · [02-开发计划表.md](../02-开发计划表.md)

---

## 背景

天枢初期设计默认用 Protobuf（详见 [adr-0007](./0007-api-spec-multi-language.md) 跨语言契约）。但实际场景下：

- **FlatBuffers**：零拷贝、Schema 严格、跨语言好、嵌入式友好（无运行时依赖）。适合高性能消息（点云、图像、大张量）
- **Protobuf**：成熟、生态丰富、跨语言好。适合配置类、控制类、跨服务消息
- **自定义 struct（POD）**：内存布局确定，memcpy 直传，无序列化开销。适合同进程内 hot path（如传感器驱动到融合）

绑死单一格式会带来：

| 痛点 | 后果 |
|---|---|
| 大消息（点云/图像）走 Protobuf | 序列化开销占消息延迟 30%+ |
| 自定义 struct 必须改 .proto | 用户迁移成本高 |
| FlatBuffers 用户被迫多包一层 Protobuf | 零拷贝优势全失 |
| 嵌入式场景 Protobuf runtime 装不下 | 必须有更轻量选项 |

天枢**不绑死任何格式**，提供统一的 `MessageConcept` 抽象，三种格式自由混用。

## 候选方案

### 方案 1：Protobuf only

强制所有消息走 Protobuf lite。

**优点**：跨语言成熟；序列化层简单。
**缺点**：大消息性能损失；自定义 struct 必须改 .proto；FlatBuffers 用户被排除。

### 方案 2：FlatBuffers only

强制所有消息走 FlatBuffers。

**优点**：零拷贝；性能好；Schema 严格。
**缺点**：小消息（控制类）有开销；生态不如 Protobuf；写入稍复杂。

### 方案 3：POD only（C++ 专属）

强制所有消息是 POD struct。

**优点**：零开销。
**缺点**：跨机/跨语言不可用；Schema 无 IDL；不可演进。

### 方案 4：统一抽象 + 三格式共存（**已选**）

提供 `MessageConcept` C++20 concept，三种格式自动特化，用户按场景自由选。

## 决策

**选方案 4**：`MessageConcept` 统一抽象，FlatBuffers / Protobuf / POD 三格式共存。

## 核心抽象

### MessageConcept（C++20 concept）

```cpp
namespace tianshu::core {

template<typename T>
concept MessageConcept = requires(const T& msg) {
  { MessageTraits<T>::name() } -> std::convertible_to<std::string_view>;
  { MessageTraits<T>::is_zero_copy() } -> std::same_as<bool>;
  { MessageTraits<T>::max_serialized_size() } -> std::convertible_to<size_t>;
} && requires(T& msg, void* buf, size_t size) {
  { MessageTraits<T>::serialize(msg, static_cast<uint8_t*>(buf), size) } -> std::same_as<size_t>;
  { MessageTraits<T>::deserialize(static_cast<const uint8_t*>(buf), size) } -> std::same_as<T*>;
};

}  // namespace tianshu::core
```

### MessageTraits 默认特化（三种格式）

```cpp
// 1. POD struct 自动特化（任何内存布局固定的类型）
template<POD T>
struct MessageTraits<T> {
  static constexpr bool is_zero_copy = true;
  static std::string_view name() { return typeid(T).name(); }  // 仅 debug
  static size_t serialize(const T& msg, uint8_t* buf, size_t size) {
    std::memcpy(buf, &msg, sizeof(T)); return sizeof(T);
  }
  static const T* deserialize(const uint8_t* buf, size_t size) {
    return size >= sizeof(T) ? reinterpret_cast<const T*>(buf) : nullptr;
  }
  static constexpr size_t max_serialized_size() { return sizeof(T); }
};

// 2. FlatBuffers 显式特化（用户用 TIANSHU_TRAITS_FLATBUFFER 宏注册）
template<flatbuffers::Table T>
struct MessageTraits<T> {
  static constexpr bool is_zero_copy = true;  // flatbuffers 直接 SHM 传 builder.GetBufferPointer
  static std::string_view name() { return "flatbuffer::" + T::TableType::name; }
  static size_t serialize(const T& msg, uint8_t* buf, size_t size);   // = memcpy builder buffer
  static const T* deserialize(const uint8_t* buf, size_t size);       // = Get##T(buf)
  static size_t max_serialized_size() { return T::max_size; }
};

// 3. Protobuf 显式特化（用户用 TIANSHU_TRAITS_PROTOBUF 宏注册）
template<google::protobuf::MessageLite T>
struct MessageTraits<T> {
  static constexpr bool is_zero_copy = false;  // protobuf 需要 serialize
  static std::string_view name() { return T::default_instance().GetTypeName(); }
  static size_t serialize(const T& msg, uint8_t* buf, size_t size) {
    return msg.SerializeToArray(buf, size) ? msg.ByteSizeLong() : 0;
  }
  static const T* deserialize(const uint8_t* buf, size_t size);  // = T().ParseFromArray
  static size_t max_serialized_size() { return T::default_instance().ByteSizeLong() * 2; }
};
```

### Reader / Writer 模板

```cpp
template<MessageConcept TMsg>
class Reader {
 public:
  bool TryFetch(TMsg** out);
  void Observe();
  // ...
};

template<MessageConcept TMsg>
class Writer {
 public:
  bool Write(const TMsg& msg);
  // ...
};
```

### 用户注册宏

```cpp
// FlatBuffers
TIANSHU_TRAITS_FLATBUFFER(CameraFrame, "tianshu.flatbuf.CameraFrame");

// Protobuf
TIANSHU_TRAITS_PROTOBUF(ControlCommand, "tianshu.proto.ControlCommand");

// POD
struct ImuData {
  double timestamp;
  double ax, ay, az;
  double gx, gy, gz;
};
TIANSHU_TRAITS_POD(ImuData, "tianshu.pod.ImuData");
```

### DSL 透明使用

```cpp
void perception_flow(Node& node) {
  // 三种格式混用，DSL 不感知
  auto camera = node.reader<CameraFrame>("/perception/front");    // FlatBuffer
  auto imu = node.reader<ImuData>("/imu/raw");                    // POD
  auto cmd = node.writer<ControlCommand>("/control/cmd");         // Protobuf

  node.on_input({camera, imu}, [&](auto c, auto i) {
    // ...
    cmd.write(make_command(...));
  });
}
```

## 传输层适配策略

Pass 4 SLA 物理规划时，编译器根据 `MessageTraits` 选择最优传输策略：

| 消息特征 | 同进程 INTRA | 同机 SHM | 跨机 RTPS |
|---|---|---|---|
| POD（is_zero_copy=true, 小） | 指针传递 | memcpy（不序列化） | **必须序列化**（用户负责或禁用） |
| POD（is_zero_copy=true, 大） | 指针传递 | memcpy | 序列化 |
| FlatBuffers（is_zero_copy=true） | 指针传递 | builder buffer memcpy | builder buffer memcpy |
| Protobuf（is_zero_copy=false） | 指针传递（同进程内 protobuf 对象） | serialize → SHM → deserialize | serialize → 网络 → deserialize |

**编译期警告**：

- POD 跨机：警告"POD struct 跨机传输不可移植（endianness / struct padding），建议改为 FlatBuffers 或 Protobuf"
- 大 Protobuf 消息（>1MB）：警告"考虑改用 FlatBuffers 提升性能"

## Profile 与消息格式

| Profile | POD | FlatBuffers | Protobuf |
|---|---|---|---|
| desktop | ✅ | ✅ | ✅ |
| server | ✅ | ✅ | ✅ |
| vehicle | ✅ | ✅ | ✅ |
| embedded | ✅ | ✅ | ⚠️（仅 lite） |
| mcu | ✅ | ⚠️（受限） | ❌（runtime 装不下） |

MCU profile 强烈推荐 POD + 少量 FlatBuffers，禁用 Protobuf。

## 多语言契约（与 ADR-0007 协同）

| 语言 | POD | FlatBuffers | Protobuf |
|---|---|---|---|
| C++ | ✅ 原生 | ✅ 原生 | ✅ 原生 |
| Python | ❌（强 C++ 布局） | ✅（flatbuffers 官方） | ✅（官方） |
| Rust | ⚠️（#[repr(C)] 限定） | ✅（flatbuffers rust） | ✅（prost） |
| Go | ❌ | ✅ | ✅（官方） |
| Node.js | ❌ | ✅ | ✅（官方） |

跨语言消息强制 FlatBuffers 或 Protobuf；POD 仅 C++/Rust 内部使用。

## MessageFactory（跨机接收端用）

跨机接收时，接收方根据 channel_name 查表反序列化：

```cpp
class MessageFactory {
 public:
  static MessageFactory& instance();

  void register_type(std::string_view name, std::function<void*(const uint8_t*, size_t)> deserializer);

  // 根据 channel_name + 收到的 bytes 反序列化为 opaque 对象
  void* deserialize(std::string_view channel_name, const uint8_t* buf, size_t size);

  // 已注册的所有类型清单
  std::vector<std::string> registered_types() const;
};
```

`TIANSHU_REGISTER_MESSAGE` 宏同时注册到 `MessageFactory` 和 DSL 类型表。

## 设计契约锁定

Phase 0 起锁定（影响所有 Reader/Writer/Transport 实现）：

| 契约 | 说明 |
|---|---|
| `MessageConcept` 概念 | C++20 concept 锁定，未来不变 |
| `MessageTraits<T>` API | name/serialize/deserialize/is_zero_copy/max_serialized_size |
| Reader/Writer 模板签名 | 不接受非 MessageConcept 类型 |
| 三种特化 | POD（自动）/ FlatBuffers（宏注册）/ Protobuf（宏注册） |
| 类型注册 | `TIANSHU_REGISTER_MESSAGE` 必须配对 channel_name |

## 影响范围

### 新增功能点（L4-CORE）

| ID | 描述 | 优先级 | Phase |
|---|---|---|---|
| L4-CORE-10 | `MessageConcept` C++20 concept + 默认特化（POD auto） | P0 | 1 |
| L4-CORE-11 | FlatBuffers `MessageTraits` 特化 + 注册宏 | P0 | 1 |
| L4-CORE-12 | Protobuf lite `MessageTraits` 特化 + 注册宏 | P0 | 1 |
| L4-CORE-13 | `MessageFactory`（按 channel_name 反序列化） | P0 | 1 |
| L4-CORE-14 | Pass 4 消息格式感知传输策略（POD/Flat/Proto → INTRA/SHM/跨机选择） | P1 | 2 |
| L4-CORE-15 | 多格式跨语言消息兼容性测试套（FlatBuffers + Protobuf 跨 Python/C++ 一致性） | P1 | 2 |

### L1-CG 扩展

| ID | 描述 | 优先级 | Phase |
|---|---|---|---|
| L1-CG-13 | codegen 按消息格式生成传输代码（POD: memcpy；FlatBuffers: 零拷贝；Protobuf: serialize） | P1 | 2 |

### ADR-0007 序列化契约修订

ADR-0007 §跨语言契约 §序列化 修订：

> **必须**：Protobuf（含 lite runtime，跨语言好）**或 FlatBuffers**（零拷贝，性能更好）
> **可选**：自定义 struct（POD，仅 C++/Rust 内部）
> **禁用**：任何 C++ 专属序列化（boost::serialization、cereal 等）
> JSON 仅用于调试和 Web，不用于核心消息

### ADR-0005 依赖治理修订

FlatBuffers 入白名单（之前未明确）：

| 依赖 | 档位 | 理由 |
|---|---|---|
| **FlatBuffers** | 🟢 允许 | 零拷贝、跨语言、嵌入式友好（header-only / lite runtime）、Apache 2.0 |

### README / 00 方案总览

核心特性加：

> **消息格式多支持** | FlatBuffers（零拷贝大消息）+ Protobuf（跨语言小消息）+ 自定义 struct（POD hot path）；`MessageConcept` 统一抽象，用户按场景自由选

## 风险与缓解

| 风险 | 缓解 |
|---|---|
| 三种格式共存增加测试矩阵 | 跨格式一致性测试套（L4-CORE-15） |
| 用户选错格式（POD 跨机 / 大 Protobuf） | 编译期警告 + lint 规则 |
| FlatBuffers 与 Protobuf 字段对齐不一致 | 提供 .fbs ↔ .proto 转换工具（Phase 3） |
| MCU 上无 FlatBuffers runtime | 受限子集；优先 POD |
| MessageTraits 性能损耗 | 模板编译期分发 + inline；非虚函数 |

## 后续可能演进

- 如果用户场景以大消息为主 → 默认 FlatBuffers，Protobuf 降为可选
- 如果 FlatBuffers schema 工具链不稳 → 引入 Cap'n Proto（同类替代）
- 如果未来需要 RDMA/GPU direct → POD + RDMA 注册到 MessageTraits
- 如果消息 schema 需要版本化演进 → 加 `SchemaVersion` 字段到 MessageTraits

## 编译期可选依赖（feature flag 机制）

天枢消息格式支持**编译期可选**，用户可完全不依赖 Protobuf / FlatBuffers 之一。

### Feature flag

```bash
# CMake
cmake -B build \
  -DTIANSHU_WITH_PROTOBUF=ON \       # 默认 ON
  -DTIANSHU_WITH_FLATBUFFERS=ON \    # 默认 ON
  -DTIANSHU_WITH_POD=ON              # 默认 ON（核心，不推荐关）

# 完全不依赖 Protobuf 的最小构建
cmake -B build -DTIANSHU_WITH_PROTOBUF=OFF

# Bazel
bazel build //... --config=with-protobuf         # 在 .bazelrc 中
bazel build //... --noincompatible_tianshu_no_protobuf  # 默认
```

### 影响范围（关闭某格式时）

| 关闭项 | 影响 |
|---|---|
| `TIANSHU_WITH_PROTOBUF=OFF` | `ProtobufMessageTraits` 特化不编译；`MessageFactory` 不注册 Protobuf 类型；不链接 `libprotobuf-lite`；DSL 模板支持但运行时报错 |
| `TIANSHU_WITH_FLATBUFFERS=OFF` | 同上，对 FlatBuffers |
| `TIANSHU_WITH_POD=OFF`（不推荐） | POD 自动特化禁用；只允许 Protobuf / FlatBuffers |

### 编译期检测

```cpp
#if TIANSHU_WITH_PROTOBUF
  auto reader = node.reader<ControlCommand>("/control");  // ✅ OK
#else
  auto reader = node.reader<ControlCommand>("/control");  // ❌ 编译错误：MessageTraits 未定义
#endif
```

用户尝试用未启用的格式时**编译期失败**，不是运行时崩溃。

### Profile 默认配置

| Profile | Protobuf | FlatBuffers | POD |
|---|---|---|---|
| desktop / server / vehicle | ✅ ON | ✅ ON | ✅ ON |
| embedded | ✅ ON | ✅ ON | ✅ ON |
| **mcu** | **❌ OFF**（runtime 装不下） | ⚠️ 受限（仅 read-only 子集） | ✅ ON |

MCU profile 默认禁用 Protobuf（详见 [ADR-0005 MCU 特殊路径](./0005-lightweight-multiplatform.md)）。

### 与 [ADR-0016 配置格式](./0016-config-format.md) 协同

类似配置格式的 feature flag 模式，天枢所有可选依赖统一用 `TIANSHU_WITH_*` CMake/Bazel option 控制，遵循 [ADR-0005 依赖治理](./0005-lightweight-multiplatform.md) 流程。

## 参考

- FlatBuffers: https://google.github.io/flatbuffers/
- Protobuf Lite: https://protobuf.dev/reference/cpp/api-docs/google.protobuf.message_lite
- C++20 Concepts: https://en.cppreference.com/w/cpp/language/constraints
