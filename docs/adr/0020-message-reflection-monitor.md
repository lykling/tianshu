# ADR-0020：消息运行期反射与 Monitor 解析（多消息格式）

- **状态**：已接受（Phase 1 已实现：POD 字段表 + FieldTreeView + DecoderRegistry + ti-monitor --decode；Phase 2 已实现：SHM sidecar schema 分发，ti-monitor 免参数自动解码）
- **日期**：2026-08-27
- **决策者**：Pride Leong
- **关联**：[adr/0008](./0008-message-format-multi.md) · [adr/0010](./0010-transport-shm-infra.md) · [adr/0014](./0014-console.md) · [adr/0015](./0015-discovery-abstraction.md)

---

## 背景

`ti-monitor`（ti 家族工具，ADR-0002 术语节）目前只显示 payload 的十六进制转储。要达到 cyber_monitor 的可用性，必须显示**解析后的字段**。难点在 TIANSHU 支持三种消息格式（ADR-0008），它们的运行期内省能力完全不同：

| 格式 | 运行期内省 | 现状 |
|---|---|---|
| Protobuf | **完备**：DescriptorPool + DynamicMessageFactory | 依赖未引入（L4-CORE-12 计划中） |
| FlatBuffers | **有条件**：`reflection.Schema` 二进制（`flatc -b --schema` 产物）+ reflection API，无需生成代码 | 依赖未引入 |
| POD struct | **零内省**：C++ 无反射 | TIANSHU_TRAITS_POD 只注册了类型名 |

同时调研了业界四种做法（详见决策依据），核心参照是 cyber_monitor 的 **schema-via-discovery** 机制。

## 候选方案

### 方案 1：仅 hex dump（现状）

无解析。用户不可接受，淘汰。

### 方案 2：schema 在工具侧预配置（用户手写映射文件）

ti-monitor 启动时加载 `channel → 解析规则` 配置。

**缺点**：每加一个 channel 要改工具配置；跨进程/跨机器发布者无法共享规则；与「发现即所见」的实时性矛盾。ROS2 `ros2 topic echo` 的教训恰恰是不能依赖本地安装的生成代码——车队环境里工具机不等于开发机。

### 方案 3：schema 随发现协议分发 + 统一解码注册表（**已选**）

扩展通道元数据：writer 声明通道时附带可选 `schema` 描述符（格式枚举 + 类型名 + 二进制 schema blob）。订阅侧（monitor / 未来的 console / record）按 `(格式, 类型名)` 查统一注册表解码。

## 决策

**选方案 3**。分四点锁定：

### 1. 通道 schema 元数据（SchemaInfo）

`ChannelConfig` / 发现记录新增可选字段：

```cpp
struct SchemaInfo {
  MessageFormat format;        // kPod / kFlatBuffers / kProtobuf（已有枚举）
  std::string type_name;       // MessageTraits<T>::name()
  std::vector<std::uint8_t> blob;  // 格式相关的二进制 schema（见下表）
};
```

| 格式 | blob 内容 | 生成方式 |
|---|---|---|
| kProtobuf | `FileDescriptorSet`（含依赖） | writer 侧从 linked descriptor 序列化（cyber `ProtoDesc` 同构） |
| kFlatBuffers | `reflection.Schema` 平面缓冲 | `flatc -b --schema xxx.fbs` 的产物，构建期生成、运行期附上 |
| kPod | TIANSHU 字段表（自有编码，见第 3 点） | `TIANSHU_TRAITS_POD_FIELDS` 宏生成 |

**关键性质**：schema 与消息**同源同版本**——发布什么类型的进程就带什么 schema，工具机零预配置。这正是 MCAP 把 Schema record 嵌进文件、cyber 把 proto_desc 塞进 RoleAttributes 的同一设计判断。

### 2. 解码注册表（DecoderRegistry，单例）

```cpp
// (format, type_name) -> decoder
class DecoderRegistry {
 public:
  // 三种内建注册路径 + 用户自定义（kCustom 格式的逃生门）
  void register_schema(const SchemaInfo& info);      // 运行期：从发现/文件加载
  FieldTreeView decode(const SchemaInfo& info,       // payload -> 字段树
                       std::span<const std::uint8_t> payload) const;
};
```

- `FieldTreeView` 是**格式无关的中间表示**（字段名/类型/值/嵌套），TUI、console、未来的 record 回放共用；工具层不再感知三种格式
- Protobuf 路径：`DescriptorPool`（先 generated pool 后动态 pool 的**两级回退**，照搬 cyber `ProtobufFactory`）+ `DynamicMessageFactory` + 反射遍历
- FlatBuffers 路径：`reflection::Schema` + `flatbuffers::reflection::IterateObjects`
- POD 路径：见第 3 点
- 查不到 schema → **降级到 hex dump**（比 cyber 的错误字符串友好；工具永远可用）

### 3. POD 字段表：TIANSHU_TRAITS_POD_FIELDS 宏

C++ 无反射，POD 的 schema 必须**编译期声明**。参照 ROS2 `rosidl_typesupport_introspection_cpp` 的 `MessageMembers` 表（字段名/偏移/类型/数量的静态数组）：

```cpp
struct ImuData { double timestamp; double ax, ay, az; };

TIANSHU_TRAITS_POD_FIELDS(ImuData, "tianshu.pod.ImuData",
    (timestamp, double)(ax, double)(ay, double)(az, double))
```

宏生成 `constexpr` 字段描述数组（name/offset/type/arity），与 `TIANSHU_TRAITS_POD` 二选一（带字段的版本优先）。**不写宏的 POD 维持现状**——只有类型名 + hex dump，不做魔法推断（对齐「POD 跨进程布局本就由作者负责」的 ADR-0008 立场）。

序列化形态：字段表编码进 `SchemaInfo.blob`，跨进程可用（field: name_len, name, type_tag, offset, count）。

### 4. 与发现协议的集成点

- Phase 1（无独立 discovery 服务）：`IntraChannelRegistry` / SHM `ChannelHeader` 旁挂 schema（SHM 段 header 预留 `schema_offset`，或独立小段 `tianshu_schema_<hash>`——实现时按段的定额成本二选一）
  - **已选定独立小段**（2026-08-28 落地）：`/tianshu_schema_<fnv1a>` 与 `/tianshu_ch_<fnv1a>` 同哈希派生。理由：零改动现有段布局（`schema_offset` 方案变更 `ChannelHeader`，破坏在飞段的 ABI）；定额成本为每 schema'd 通道一页。发布语义：blob 先写、magic release-store 收尾（读者 acquire）；首写者胜出；段随 writer 进程存活（与数据段同生命周期）
- Phase 2（ADR-0015 DiscoveryBackend 落地）：schema 随 join 通告分发（cyber `DisposeJoin → RegisterMessage` 同构）
- **通道类型元信息不再需要手传**：`ti monitor /sensing/imu` 自动拿到 `kPod + tianshu.pod.ImuData + 字段表`

## 决策依据（业界对照）

| 系统 | 类型名来源 | schema 位置 | 非 proto 支持 |
|---|---|---|---|
| cyber_monitor | 发现协议（RoleAttributes） | **发现载荷内**（proto_desc）+ generated pool 回退 | 无（错误字符串） |
| ROS2 echo | 图发现（rmw） | 工具机已安装的生成代码 | 仅 ROS 类型 |
| MCAP/Foxglove | Channel record | **文件/流内嵌**（protobuf=FileDescriptorSet、fbs=reflection.Schema） | 任意注册编码 |
| PlotJuggler | 连接头/bag | schema 文本，运行期编译 | .msg/IDL |

四个系统殊途同归的一点：**schema 与数据同通道分发，不依赖工具侧预配置**。方案 3 是该原则在 TIANSHU 三格式语境下的投影。

cyber 关键实现参照（上游 `ApolloAuto/apollo@d53aa3d`）：
- 懒解码 + shared_ptr 快照：`cyber/tools/cyber_monitor/general_channel_message.cc` L228-291
- 两级 pool：`cyber/message/protobuf_factory.cc` L170-211
- 发现分发：`service_discovery/specific_manager/channel_manager.cc` L271-286（JOIN → RegisterMessage）
- 反射渲染器：`cyber/tools/cyber_monitor/general_message_base.cc` L131-322

## 影响范围

### 分期

- **本 ADR 只锁架构**（SchemaInfo 形状、注册表接口、POD 字段表宏、分发原则）
- **实现分期**：Phase 1（已完成）POD 字段表 + FieldTreeView + ti-monitor 字段渲染；Phase 2（已完成）SHM sidecar schema 分发 + monitor 免参数自动解码；Protobuf/FlatBuffers 解码随 L4-CORE-11/12 依赖引入后补齐（blob 格式已定，接口不变）
- 新增功能点：L4-TRANS-33（通道 schema 元数据分发）、F-L4-CONSOLE 补充（monitor 解析渲染）——进开发计划

### 与既有 ADR 的关系

- ADR-0008：无冲突；POD 字段表是其「POD 跨语言不可用」立场的细化（有字段表的 POD 仍不承诺跨语言，只承诺可观测）
- ADR-0015：schema 分发是 discovery 的第一个硬需求，佐证其抽象必要性
- ADR-0014：console/monitor/record 共用 DecoderRegistry，本 ADR 是其底层依赖

## 风险与缓解

| 风险 | 缓解 |
|---|---|
| schema blob 体积（大 proto 的 FileDescriptorSet 可达数百 KB） | 按 channel 分发一次（非每消息）；SHM 段独立挂载；超限时降级为类型名 + hex |
| POD 作者忘写字段宏 | 静态断言只在校验 traits 时提示；monitor 侧 hex dump 兜底，不阻断 |
| 三格式解码器的维护成本 | FieldTreeView 隔离——工具层只消费 IR；解码器各 <300 行，有 MCAP/cyber 参照实现 |
| 版本漂移（schema 与发布代码不同步） | schema 由 writer 进程自身的类型生成（同源原则），不做文件系统旁路加载——杜绝错配 |

## 后续可能演进

- `tianshu-ctl inspect` 消费同一注册表（离线看 record 里的 schema）
- DSL 层（ADR-0001）从 FieldTreeView 自动生成 plot 通道（数值字段直连绘图）
- kCustom 格式的用户自注册解码回调（第三方消息格式的逃生门）
