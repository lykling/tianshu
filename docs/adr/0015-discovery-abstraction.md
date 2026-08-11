# ADR-0015：服务发现抽象（DiscoveryBackend）

- **状态**：已接受
- **日期**：2026-08-10
- **决策者**：Pride Leong
- **关联**：[adr/0010](./0010-transport-shm-infra.md) · [adr/0013](./0013-cross-machine-transport.md)

---

## 背景

用户要求：服务发现也要**抽象接口**，与 TransportBackend 抽象一致，支持替换不同实现（详见用户对 [eval/0001](../evaluation/0001-cross-machine-transport.md) Fork A3 的反馈）。

[ADR-0010](./0010-transport-shm-infra.md) 已定义 TransportBackend 抽象。本 ADR 定义对称的 DiscoveryBackend 抽象，让 transport 和 discovery 都可独立替换。

## 决策

定义 `DiscoveryBackend` 抽象接口，多个实现可插拔：

```cpp
namespace tianshu::discovery {

enum class BackendType {
  ZENOH,        // 基于 Zenoh 内置 discovery（详见 ADR-0013）
  STATIC,       // 静态配置（编译期 / 启动期固定）
  MULTICAST,    // 自研多播（备用方案）
  CENTRAL,      // 集中协调（如 etcd / Consul / 自研）
  CUSTOM,       // 用户自定义
};

struct PeerInfo {
  std::string node_id;        // 节点唯一 ID
  std::string host;
  uint16_t port;
  int process_id;
  std::vector<std::string> published_channels;
  std::vector<std::string> subscribed_channels;
  std::chrono::system_clock::time_point last_seen;
};

class DiscoveryBackend {
 public:
  virtual ~DiscoveryBackend() = default;

  // 生命周期
  virtual Status start(const DiscoveryConfig& cfg) = 0;
  virtual Status stop() = 0;

  // 注册本节点发布的 channel
  virtual Status register_publisher(const std::string& channel) = 0;
  virtual Status unregister_publisher(const std::string& channel) = 0;

  // 订阅本节点感兴趣的 channel（触发回调）
  virtual Status register_subscriber(const std::string& channel) = 0;
  virtual Status unregister_subscriber(const std::string& channel) = 0;

  // 事件回调（peer 上线/下线、channel 出现/消失）
  using PeerCallback = std::function<void(const PeerInfo&)>;
  using ChannelCallback = std::function<void(const std::string& channel, const PeerInfo& peer)>;
  virtual void on_peer_online(PeerCallback cb) = 0;
  virtual void on_peer_offline(PeerCallback cb) = 0;
  virtual void on_channel_published(ChannelCallback cb) = 0;
  virtual void on_channel_unpublished(ChannelCallback cb) = 0;

  // 查询当前已知状态
  virtual std::vector<PeerInfo> list_peers() const = 0;
  virtual std::vector<PeerInfo> list_publishers(const std::string& channel) const = 0;
  virtual std::vector<PeerInfo> list_subscribers(const std::string& channel) const = 0;

  // 能力
  virtual BackendType type() const = 0;
  virtual bool supports_remote() const = 0;  // 跨机？
};

// Registry（同 TransportRegistry）
class DiscoveryRegistry {
 public:
  static DiscoveryRegistry& instance();
  void register_backend(BackendType type, BackendFactory factory);
  DiscoveryBackend* get(BackendType type);
};

}  // namespace tianshu::discovery
```

## 内置实现

| Backend | 适用场景 | 实现 |
|---|---|---|
| `ZenohDiscoveryBackend` | vehicle / server / desktop / embedded | 包装 Zenoh 内置 discovery（gossip + 多播） |
| `StaticDiscoveryBackend` | mcu / 简单部署 | 启动时从配置文件读 peer 列表 |
| `MulticastDiscoveryBackend` | 备用方案 | 自研 UDP 多播（参考 RTPS SPDP） |
| `CentralDiscoveryBackend` | 大规模集群 | 集中协调（etcd / Consul / 自研 HTTP） |

## HYBRID 自动选择（默认）

```cpp
class HybridDiscovery : public DiscoveryBackend {
  // 同机：进程内通信（自动检测 process_id）
  // 同机多进程：Zenoh 同机模式（unix socket）
  // 跨机：Zenoh 多播 / router
  // MCU：StaticDiscovery
};
```

**编译器集成**：Pass 1 分析阶段，编译器查询 DiscoveryBackend 决定每个 channel 走哪个 transport backend（INTRA/SHM/Zenoh）。

## 配置

```toml
# tianshu.toml
[discovery]
backend = "HYBRID"  # HYBRID / ZENOH / STATIC / MULTICAST / CENTRAL
node_id = "perception_01"
announce_interval_ms = 1000
timeout_ms = 5000

[discovery.zenoh]
mode = "peer"        # peer / client / router
connect = ["tcp/192.168.1.10:7447"]
listen = ["tcp/0.0.0.0:7447"]
multicast = true

[discovery.static]
peers = [
  { node_id = "planning_01", host = "192.168.1.11", port = 7447 },
  { node_id = "control_01", host = "192.168.1.12", port = 7447 },
]
```

## Profile 启用矩阵

| Profile | HYBRID | Zenoh | Static | Multicast | Central |
|---|---|---|---|---|---|
| desktop | ✅ | ✅ | ⚠️ | ⚠️ | ❌ |
| server | ✅ | ✅ | ⚠️ | ⚠️ | ✅（大规模） |
| vehicle | ✅ | ✅ | ⚠️ | ⚠️ | ❌ |
| embedded | ✅ | ⚠️ | ✅ | ⚠️ | ❌ |
| mcu | ⚠️（仅同机） | ⚠️（pico） | ✅ | ❌ | ❌ |

## 影响范围

### 新增框架 F-L4-DISC（替代部分 F-L4-SD）

| ID | 描述 | 优先级 | Phase |
|---|---|---|---|
| L4-DISC-1 | `DiscoveryBackend` 抽象接口 + Registry | P0 | 1 |
| L4-DISC-2 | `ZenohDiscoveryBackend` 实现（包装 Zenoh gossip） | P1 | 2 |
| L4-DISC-3 | `StaticDiscoveryBackend`（从配置文件加载） | P0 | 1 |
| L4-DISC-4 | `HybridDiscovery` 自动选择策略 | P0 | 1 |
| L4-DISC-5 | Peer/Channel 事件回调机制 | P0 | 1 |
| L4-DISC-6 | `MulticastDiscoveryBackend`（备用） | P2 | 3 |
| L4-DISC-7 | `CentralDiscoveryBackend`（etcd / Consul 集成） | P2 | 3 |

**注**：原 L4-SD（ServiceDiscovery）框架的内容**整合进 F-L4-DISC**，L4-SD-1..6 重命名为 L4-DISC-*。

## 与现有架构协同

| ADR | 协同点 |
|---|---|
| [adr/0010 transport](./0010-transport-shm-infra.md) | TransportBackend 通过 DiscoveryBackend 获取 peer 分布，决定 backend 选择 |
| [adr/0013 跨机](./0013-cross-machine-transport.md) | Zenoh 内置 discovery 是 ZenohDiscoveryBackend 的实现基础 |
| [adr/0007 多语言](./0007-api-spec-multi-language.md) | DiscoveryBackend 通过 C ABI 暴露给多语言 SDK |

## 风险与缓解

| 风险 | 缓解 |
|---|---|
| Zenoh discovery 在大规模场景收敛慢 | 切换 CentralDiscovery（etcd）|
| 多 backend 共存导致状态不一致 | 单一来源（HYBRID 自动选一个）+ 严格事件顺序 |
| mcu 静态配置变更需重启 | 提供 mainboard 热重启机制（保留消息缓冲） |

## 后续可能演进

- 如果未来出现 k8s 原生 discovery → 加 K8sDiscoveryBackend
- 如果未来出现 mDNS / DNS-SD 强需求 → 加 MdnsDiscoveryBackend
- 如果跨数据中心场景 → 加 FederatedDiscoveryBackend
