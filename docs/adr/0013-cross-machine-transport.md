# ADR-0013：跨机 Transport（自研 SHM + Zenoh，含接口抽象）

- **状态**：已接受（升级自 [evaluation/0001](../evaluation/0001-cross-machine-transport.md)）
- **日期**：2026-08-10
- **决策者**：Pride Leong
- **关联**：[adr/0005](./0005-lightweight-multiplatform.md) · [adr/0007](./0007-api-spec-multi-language.md) · [adr/0010](./0010-transport-shm-infra.md) · [adr/0015](./0015-discovery-abstraction.md)

---

## 决策

按 [evaluation/0001](../evaluation/0001-cross-machine-transport.md) 推荐方案 A 落地：

| Fork | 选定 |
|---|---|
| 主路线 | **A**：自研 SHM（同机）+ Zenoh（跨机） |
| Zenoh 许可证 | **A1**：Apache-2.0 子许可（双许可中选 Apache-2.0） |
| Discovery | 走 Zenoh 内置 discovery（详见 [ADR-0015](./0015-discovery-abstraction.md) 服务发现抽象） |
| MCU 跨机 | **M2**：MCU 通过 Zenoh-pico 子集支持跨机，方案与主线一致 |

## 设计要点

### 1. 跨机走 `ZenohBackend`，实现 [TransportBackend](./0010-transport-shm-infra.md) 接口

```cpp
namespace tianshu::transport::zenoh {

class ZenohBackend : public TransportBackend {
 public:
  BackendType type() const override { return BackendType::ZENOH; }
  bool supports_zero_copy() const override { return false; }  // 跨机不可避免序列化
  bool supports_remote() const override { return true; }
  bool supports_msg_format(MessageFormat f) const override;

  Result<WriterHandle> create_writer(const ChannelConfig& cfg) override;
  Result<ReaderHandle> create_reader(const ChannelConfig& cfg) override;
  Status configure(const BackendConfig& cfg) override;
};

}  // namespace tianshu::transport::zenoh
```

**关键**：用户/编译器看到的是统一 `TransportBackend` 接口，Zenoh 实现细节封装在 `.cc` 文件中，未来可零成本替换为 CycloneDDS / 自研。

### 2. MCU profile 通过 Zenoh-pico

`ZenohBackend` 有两个实现变体：

| 实现 | 适用 profile | 库 |
|---|---|---|
| `ZenohBackend`（完整） | desktop / server / vehicle / embedded | `libzenohc`（Rust 编译的 C ABI） |
| `ZenohPicoBackend`（嵌入式子集） | mcu | `libzenohpico`（C 实现，< 100KB） |

两者通过同一 `TransportBackend` 接口暴露，**用户代码无感知**。

### 3. 编译期可选（feature flag）

通过 CMake/Bazel option 控制：

```bash
# CMake
cmake -B build -DTIANSHU_WITH_ZENOH=ON     # 默认 ON（vehicle/server/desktop）
cmake -B build -DTIANSHU_WITH_ZENOH=OFF    # 不需要跨机时禁用（如某些 embedded 配置）

# Bazel
bazel build //... --config=with-zenoh      # 在 .bazelrc 中
bazel build //...                           # 默认（按 profile）
```

`TIANSHU_WITH_ZENOH=OFF` 时：

- `ZenohBackend` 不编译
- 跨机 transport 不可用（运行期报错）
- 整个二进制不链接 `libzenohc` / `libzenohpico`

### 4. 许可证合规

Zenoh 双许可 `EPL-2.0 OR Apache-2.0`，天枢选用 Apache-2.0 子许可：

- 二进制发行物含 Zenoh Apache-2.0 版本
- `NOTICE` 文件声明 Zenoh 使用
- 与天枢 Apache-2.0 许可证（详见 [ADR-0017](./0017-license.md)）兼容

### 5. Profile 启用矩阵

| Profile | Zenoh 完整 | Zenoh-pico | 备注 |
|---|---|---|---|
| desktop | ✅ 默认 | - | 开发调试 |
| server | ✅ 默认 | - | 仿真/训练 |
| vehicle | ✅ 默认 | - | 跨 ECU 通信 |
| embedded | ⚠️ 可选 | - | 资源够时启用 |
| mcu | - | ✅ 默认 | 仅 Zenoh-pico 子集 |

## 影响范围

### 新增/修订功能点

| ID | 描述 | 优先级 | Phase |
|---|---|---|---|
| L4-TRANS-6（修订） | `ZenohBackend` 完整实现（替代原 RTPS 选项） | P1 | 2 |
| L4-TRANS-28（新） | `libzenohc` 集成 + `ALLOWED_DEPS` 加入（Apache-2.0） | P0 | 1-2 |
| L4-TRANS-29（新） | `ZenohPicoBackend` for MCU | P2 | 3 |
| L4-TRANS-30（新） | feature flag `TIANSHU_WITH_ZENOH` CMake/Bazel 配置 | P0 | 0-1 |
| L4-TRANS-31（新） | Zenoh QoS 映射（reliable/best_effort → Zenoh consistency） | P1 | 2 |
| L4-TRANS-32（新） | Zenoh discovery 集成（详见 [ADR-0015](./0015-discovery-abstraction.md)） | P1 | 2 |

### 工作量调整

- 原 L4-TRANS-6 估算 6 点（RTPS 自研）→ 改为 4 点（Zenoh 包装，省协议实现）
- 新增 L4-TRANS-28..32 共 ~12 点

## 风险与缓解

| 风险 | 缓解 |
|---|---|
| Zenoh Apache-2.0 子许可未来变更 | 持续跟踪 Eclipse Zenoh 项目；保留切换 CycloneDDS 的能力（接口抽象） |
| Zenoh 性能在某些场景退化 | benchmark 套件持续监控；fallback 到 CycloneDDS |
| libzenohc 二进制体积大（~2MB） | feature flag 可关闭；embedded profile 可选 |
| Zenoh-pico 功能受限 | MCU profile 接受子集；不能用的高阶功能（如 routing）改用静态配置 |
| Zenoh 跨机延迟（> 1ms） | 仅 hot path 用 SHM；跨机场景接受 ms 级延迟 |

## 后续可能演进

- 如果 Zenoh 出现重大问题（停止维护 / 性能退化）→ 切换到 CycloneDDS（同接口）
- 如果 RUST 生态强烈需求 → 评估直接用 Zenoh Rust API（不走 C ABI）
- 如果未来需要 RDMA / GPU direct → 加新 backend（同接口）
- 如果 QUIC 场景强烈 → 加 QuicBackend（同接口）
