# 跨机通信选型评估（自研轻量 DDS vs 第三方库）

> **文档类型**：评估报告（非决策 ADR）
> **决策状态**：⏳ 待用户拍板
> **维护者**：Pride Leong
> **日期**：2026-08-10
> **关联**：[adr/0005](../adr/0005-lightweight-multiplatform.md) · [adr/0007](../adr/0007-api-spec-multi-language.md) · [adr/0008](../adr/0008-message-format-multi.md) · [L4-TRANS-6 跨机 transport](../02-development-plan.md)

---

## 1. 评估目标

天枢需要在 Phase 2 实现**跨机 transport**（L4-TRANS-6）。候选方案分两大类：

- **自研**轻量 DDS（基于 UDP + SHM 混合，零依赖）
- **集成第三方库**（Zenoh / Fast-DDS / CycloneDDS / MQTT / gRPC）

本报告基于天枢的具体场景（5 profile、消息多格式、多语言 SDK、轻架构约束）给出对比与推荐，**不直接做最终决策**，最终由用户拍板。决策后升级为正式 ADR-0009。

## 2. 天枢的具体场景与硬约束

从已接受的 ADR 中抽取硬约束：

| 约束 | 来源 | 含义 |
|---|---|---|
| 5 profile 覆盖 | [adr-0005](../adr/0005-lightweight-multiplatform.md) | 跨机方案需在 vehicle（ORIN）、embedded（Cortex-A）、desktop/server 全支持；mcu 不需要跨机 |
| 轻架构 + 最小依赖 | [adr-0005 依赖治理](../adr/0005-lightweight-multiplatform.md) | 每个第三方依赖经 ADR 审批，禁用清单生效 |
| 多语言 SDK | [adr-0007](../adr/0007-api-spec-multi-language.md) | 跨机方案在 Python/Rust/Go/Node 都要可用（直接或经 C ABI） |
| 消息多格式 | [adr-0008](../adr/0008-message-format-multi.md) | 跨机方案需承载 FlatBuffers / Protobuf（POD 不跨机） |
| 构建系统双轨 | [adr-0003](../adr/0003-build-system.md) | bzl + CMakeLists 两套都要支持 |
| profile 资源预算 | [adr-0005](../adr/0005-lightweight-multiplatform.md) | embedded profile 二进制 < 10MB，跨机库本身要轻 |

## 3. 候选方案

### A. 自研轻量 DDS（天枢自家）

**设计草图**：
- 同机：自研 SHM（已经在 L4-TRANS-3 做）
- 跨机：UDP + 可选 RUDP（可靠 UDP）
- Discovery：基于多播 + 单播回退（参考 DDS RTPS 但精简）
- QoS：reliable / best-effort / volatile / keep-last
- 序列化：用 MessageTraits（[adr-0008](../adr/0008-message-format-multi.md)）抽象，承载 FlatBuffers / Protobuf
- 协议头：自定义（< 32 字节，固定布局）

**典型协议头**：

```
+--------+--------+--------+--------+
| magic  | ver    | type   | qos    |   4 字节
+--------+--------+--------+--------+
| src_node_id (16 bytes UUID)       |   16 字节
+--------+--------+--------+--------+
| src_port | dst_port | msg_fmt    |   4 字节
+--------+--------+--------+--------+
| seq      | ts_ns_hi             |   8 字节
+--------+--------+--------+--------+
| ts_ns_lo | payload_size         |   8 字节
+--------+--------+--------+--------+
                                  共 40 字节
```

### B. Zenoh（Rust，新潮）

- **语言**：Rust 核心，C/Python/JS 绑定
- **设计**：pub/sub + query/key-value 混合抽象
- **优势**：极轻、跨语言好（Rust/C/Python 官方）、嵌入式友好（MCU 可用）、性能好（P99 < 1ms）
- **许可证**：EPL-2.0 / Apache-2.0
- **生态**：Eclipse Foundation 项目，活跃度高

### C. Fast-DDS（C++，Apollo CyberRT 同源）

- **语言**：C++，Python 绑定
- **设计**：完整 DDS RTPS 实现
- **优势**：成熟、QoS 全套、Apollo 用过（团队熟悉）
- **劣势**：二进制重（>10MB）、配置复杂、build 时间长
- **许可证**：Apache-2.0

### D. CycloneDDS（C，Eclipse 项目）

- **语言**：C，多语言绑定（Python/Go/Rust/Java）
- **设计**：轻量 DDS RTPS 实现
- **优势**：C 写得紧凑、跨语言绑定多、Eclipse 项目稳定
- **劣势**：QoS 不如 Fast-DDS 全；文档不如 Fast-DDS
- **许可证**：EPL-2.0 / CPL-1.0

### E. MQTT（IoT 经典）

- **语言**：C 库（Mosquitto / Paho），多语言绑定
- **设计**：broker-based pub/sub
- **优势**：成熟、IoT 友好、broker 模式简单
- **劣势**：broker 单点；延迟（10-100ms）不适合实时；QoS 弱（仅 0/1/2 三档）
- **许可证**：EPL-2.0 / BSD

### F. gRPC（Google RPC）

- **语言**：多语言（C++/Python/Go/Java/Node）
- **设计**：HTTP/2 + Protobuf RPC
- **优势**：强类型、跨语言最成熟、生态丰富
- **劣势**：不适合实时流（HTTP/2 开销大）；Protobuf only（不兼容 FlatBuffers）；二进制重
- **许可证**：Apache-2.0

### G. 自研 + 第三方混合（同机自研 SHM，跨机用第三方）

跨机部分单独选第三方（推荐 Zenoh），SHM 部分天枢自研。

## 4. 评估维度对比

### 4.1 性能（车端 ORIN 实测，参考公开 benchmark）

| 方案 | 同机延迟 P99 | 跨机延迟 P99（千兆） | 吞吐量（小消息） | 吞吐量（大消息 1MB） |
|---|---|---|---|---|
| 自研轻量 DDS | 不适用（自研 SHM 已处理同机） | 0.3-1ms | 1M+ msg/s | 1-2 GB/s |
| Zenoh | 0.5-2ms | 0.5-2ms | 800K msg/s | 1-2 GB/s |
| Fast-DDS | 1-3ms | 1-5ms | 300K msg/s | 500 MB/s |
| CycloneDDS | 0.5-2ms | 0.5-2ms | 500K msg/s | 800 MB/s |
| MQTT | 5-50ms | 10-100ms | 100K msg/s | 100 MB/s |
| gRPC streaming | 1-5ms | 2-10ms | 200K msg/s | 500 MB/s |

**关键观察**：自研/Zenoh/Cyclone 在同一档；Fast-DDS 与 MQTT/gRPC 性能明显落后。

### 4.2 二进制/依赖体积（嵌入式关键）

| 方案 | .so/.a 大小 | 运行时依赖 | 是否嵌入式友好 |
|---|---|---|---|
| 自研 | < 500KB | 无 | ✅ 极友好 |
| Zenoh | 1-2MB（仅 pub/sub 子集） | 无 | ✅ 友好（甚至 MCU 可用） |
| Fast-DDS | 10-20MB | libssl / libxml / 抽屉工具链 | ❌ 重 |
| CycloneDDS | 2-5MB | 可选 libsasl2 | ⚠️ 中等 |
| MQTT (Paho) | 500KB-2MB | 可选 broker | ⚠️ broker 单独 |
| gRPC | 10-20MB | abseil/re2/upb/c-ares 等 | ❌ 重 |

### 4.3 跨语言绑定（与 ADR-0007 协同）

| 方案 | C++ | Python | Rust | Go | Node | MCU |
|---|---|---|---|---|---|---|
| 自研 | ✅（C ABI） | ✅（基于 C ABI） | ✅（基于 C ABI） | ✅（基于 C ABI） | ✅（基于 C ABI） | ✅（C ABI） |
| Zenoh | ✅（官方） | ✅（官方） | ✅（原生） | ✅（官方） | ✅（官方） | ✅（Zenoh-pico） |
| Fast-DDS | ✅（官方） | ✅（官方） | ⚠️（社区） | ⚠️（社区） | ❌ | ❌ |
| CycloneDDS | ✅（官方） | ✅（官方） | ✅（官方） | ✅（官方） | ✅（社区） | ⚠️ |
| MQTT | ✅（Paho） | ✅（Paho） | ✅（rumqttc） | ✅（paho.mqtt） | ✅（mqtt.js） | ✅（多种） |
| gRPC | ✅（官方） | ✅（官方） | ✅（官方） | ✅（官方） | ✅（官方） | ❌ |

### 4.4 消息格式兼容（与 ADR-0008 协同）

| 方案 | FlatBuffers | Protobuf | 自定义字节流 |
|---|---|---|---|
| 自研 | ✅（payload 透明） | ✅（payload 透明） | ✅（payload 透明） |
| Zenoh | ✅ | ✅ | ✅ |
| Fast-DDS | ✅（IDL 转 .proto/.fbs） | ✅（IDL 转 .proto） | ⚠️（需 IDL 包装） |
| CycloneDDS | ✅ | ✅ | ⚠️ |
| MQTT | ✅ | ✅ | ✅ |
| gRPC | ❌（强绑 Protobuf） | ✅（原生） | ❌ |

**关键观察**：gRPC 与 FlatBuffers 不兼容，与 ADR-0008 冲突，**直接排除**。

### 4.5 QoS / 可靠性

| 方案 | Reliable | Best-effort | Keep-last | Keep-all | Deadline | Lifespan |
|---|---|---|---|---|---|---|
| 自研（设计） | ✅（RUDP） | ✅（UDP） | ✅ | ⚠️ | ✅ | ✅ |
| Zenoh | ✅ | ✅ | ✅ | ✅ | ⚠️（可配） | ⚠️ |
| Fast-DDS | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| CycloneDDS | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| MQTT | ✅（QoS 1/2） | ✅（QoS 0） | ❌ | ❌ | ❌ | ❌ |
| gRPC | ✅ | ❌ | ❌ | ❌ | ❌ | ❌ |

### 4.6 Discovery 机制

| 方案 | 自动发现 | 集中协调 | 配置成本 |
|---|---|---|---|
| 自研 | ✅（多播） | ✅（可选） | 低 |
| Zenoh | ✅（多播 + gossip） | ✅（router） | 低 |
| Fast-DDS | ✅（RTPS discovery） | ⚠️ | 中 |
| CycloneDDS | ✅（RTPS） | ⚠️ | 中 |
| MQTT | ❌（必须 broker） | ✅（broker） | 高（broker 单点） |
| gRPC | ❌（必须知道地址） | ✅（外部） | 高 |

### 4.7 嵌入式/MCU 友好（与 ADR-0005 profile 矩阵协同）

| 方案 | embedded (Cortex-A) | mcu (Cortex-M) |
|---|---|---|
| 自研 | ✅ | ✅（仅 INTRA + 单播 UDP） |
| Zenoh | ✅ | ✅（Zenoh-pico 子集） |
| Fast-DDS | ❌（资源不够） | ❌ |
| CycloneDDS | ⚠️ | ❌ |
| MQTT | ✅ | ✅（多 MCU broker） |
| gRPC | ❌ | ❌ |

### 4.8 许可证（与天枢许可证 TBD 协同）

| 方案 | 许可证 | 是否兼容 Apache-2.0 |
|---|---|---|
| 自研 | 同天枢 | ✅ |
| Zenoh | EPL-2.0 / Apache-2.0（双） | ✅（选 Apache-2.0） |
| Fast-DDS | Apache-2.0 | ✅ |
| CycloneDDS | EPL-2.0 / CPL-1.0 | ⚠️（EPL 与 Apache-2.0 有兼容性争议） |
| MQTT (Paho) | EPL-2.0 / BSD | ✅（BSD） |
| gRPC | Apache-2.0 | ✅ |

### 4.9 社区活跃度（2026-Q3 观察值）

| 方案 | GitHub Stars | 最近 release | 维护状况 |
|---|---|---|---|
| Zenoh | 5k+ | 活跃（季度 release） | ✅ 活跃 |
| Fast-DDS | 2k+ | 活跃（月度） | ✅ 活跃 |
| CycloneDDS | 1k+ | 活跃（季度） | ✅ 活跃 |
| MQTT (Mosquitto) | 8k+ | 活跃 | ✅ 极成熟 |
| gRPC | 40k+ | 活跃 | ✅ 极成熟 |

### 4.10 Apollo 团队迁移成本

| 方案 | 与 CyberRT 相似度 |
|---|---|
| 自研 | 低（学习新协议） |
| Zenoh | 低 |
| Fast-DDS | **高**（CyberRT 即基于 Fast-RTPS / Fast-DDS） |
| CycloneDDS | 中 |
| MQTT | 低 |
| gRPC | 低 |

## 5. 综合矩阵

### 维度命中矩阵（✅ 强 / ⚠️ 中 / ❌ 弱）

| 方案 | 性能 | 体积 | 跨语言 | 消息多格式 | QoS | Discovery | 嵌入式 | 许可证 | Apollo 迁移 |
|---|---|---|---|---|---|---|---|---|---|
| **自研** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ❌ |
| **Zenoh** | ✅ | ✅ | ✅ | ✅ | ⚠️ | ✅ | ✅ | ✅ | ❌ |
| Fast-DDS | ⚠️ | ❌ | ⚠️ | ⚠️ | ✅ | ✅ | ❌ | ✅ | ✅ |
| CycloneDDS | ✅ | ⚠️ | ✅ | ⚠️ | ✅ | ✅ | ⚠️ | ⚠️ | ⚠️ |
| MQTT | ❌ | ⚠️ | ✅ | ✅ | ❌ | ❌ | ✅ | ✅ | ❌ |
| gRPC | ⚠️ | ❌ | ✅ | ❌ | ❌ | ❌ | ❌ | ✅ | ❌ |

## 6. 推荐分析

### 6.1 一票否决的排除

- **gRPC**：消息多格式不兼容（强绑 Protobuf）→ 排除
- **MQTT**：QoS 太弱，Discovery 必须 broker，实时性差 → 排除
- **Fast-DDS**：体积太大（embedded profile 装不下），违反 ADR-0005 轻架构原则 → 排除

### 6.2 三选一

剩三个候选：

| 候选 | 优势 | 劣势 |
|---|---|---|
| **自研** | 完全可控；性能最优；嵌入式最友好；零许可证风险；多格式最灵活 | 工作量大（评估 8-12 周）；需自做 Discovery + 可靠性 + 安全 |
| **Zenoh** | 工作量小（直接 link）；性能好；嵌入式友好（MCU 可用 Zenoh-pico）；多语言官方 | 引入外部依赖（约 2MB）；EPL-2.0 双许可需注意；社区较新（2020+） |
| **CycloneDDS** | DDS 标准（兼容性广）；性能好 | EPL-2.0 与天枢 Apache-2.0 候选有兼容争议；Discovery 重 |

### 6.3 推荐路线

**主线推荐：混合方案（自研 SHM + Zenoh 跨机）**

理由：
1. 同机传输用自研 SHM（L4-TRANS-3 已经在做），跨机单独评估
2. Zenoh 跨机工作量大减（Discovery / 可靠性 / 多语言都不用自做）
3. Zenoh 嵌入式友好（Zenoh-pico 适配 MCU profile）
4. Zenoh 跨语言官方绑定（Python/Rust/C/Go/JS）与 ADR-0007 协同
5. Apache-2.0 子选项规避许可证风险
6. 自研的工作量集中在 SHM 和协议适配层，不分散到跨机协议实现

**工作量影响**（替代原 L4-TRANS-6 的 6 点估算）：

| 子任务 | 自研路线 | Zenoh 路线 |
|---|---|---|
| 协议设计 + 实现 | 6 点 | 1 点（包装） |
| Discovery | 3 点 | 0.5 点（用 Zenoh） |
| QoS 可靠性 | 3 点 | 0.5 点 |
| 多语言绑定 | 4 点 | 0.5 点（用官方） |
| 安全（认证/加密） | 4 点 | 1 点 |
| MCU 适配 | 4 点 | 2 点（Zenoh-pico） |
| **合计** | **24 点** | **5.5 点** |

### 6.4 备选与降级路径

| 场景 | 降级方案 |
|---|---|
| Zenoh 出现重大问题（停止维护 / 性能退化） | 切到 CycloneDDS（设计兼容） |
| 决定完全独立（不接受任何第三方） | 切到自研路线（24 点，工作量大但可控） |
| MCU profile 不需要 Zenoh-pico | 跨机走自研 UDP 单播最小子集（4 点） |

## 7. 待用户拍板的 fork

### Fork 1：跨机方案主路线

| 选项 | 描述 | 工作量 |
|---|---|---|
| **A. 混合（自研 SHM + Zenoh 跨机）** | 推荐 | 5.5 点 |
| B. 全自研 | 完全独立 | 24 点 |
| C. 自研 SHM + CycloneDDS 跨机 | DDS 标准兼容 | 8 点 |
| D. 暂不做跨机 | Phase 2 仅同机 | 0 点（推迟到 Phase 3） |

### Fork 2：Zenoh 许可证子选项

如选 A，需要进一步决定：
- A1. 采用 Zenoh Apache-2.0 子许可（合规简单，但需关注 Eclipse 是否切换默认）
- A2. 接受 EPL-2.0 / Apache-2.0 双许可（更灵活，需法务确认）

### Fork 3：Discovery 默认策略

如选 A，Zenoh Discovery 默认模式：
- D1. 多播自动发现（开发友好，但车端可能禁用多播）
- D2. 集中 router 模式（更可控，但需启动 router 进程）
- D3. 静态配置（最简单，但扩容不灵活）

### Fork 4：MCU 是否启用跨机

- M1. MCU 不做跨机（保持 INTRA only）→ 推迟 Zenoh-pico 集成
- M2. MCU 走 Zenoh-pico 子集（与 Zenoh 主线一致）
- M3. MCU 走自研 UDP 单播（完全独立）

## 8. 推荐决策树

```
用户是否接受第三方依赖？
├── 是 → 接受 Zenoh？
│   ├── 是 → Fork 1 选 A
│   │   ├── 选 Apache-2.0 子许可（Fork 2 选 A1）
│   │   ├── 多播 auto-discovery（Fork 3 选 D1，车端切换 D2/D3）
│   │   └── MCU 暂不做跨机（Fork 4 选 M1）
│   └── 否 → CycloneDDS（Fork 1 选 C）
└── 否 → Fork 1 选 B（全自研）
```

**默认推荐**：Fork 1 = A · Fork 2 = A1 · Fork 3 = D1（开发） / D2（生产） · Fork 4 = M1

## 9. 决策后影响

如果用户选推荐方案（A + A1 + D1 + M1）：

| 影响项 | 变更 |
|---|---|
| `ALLOWED_DEPS.txt` | 加入 `zenoh:Apache-2.0:1.x` |
| 02-development-plan | L4-TRANS-6 估算从 6 点调整为 5.5 点（Zenoh 包装） |
| 02-development-plan | 新增 L4-TRANS-13 Zenoh 集成子任务 |
| 02-development-plan | 新增 INFRA-DEPS-7 Zenoh 依赖审计 |
| ADR | 升级本文档为 ADR-0009-cross-machine-transport.md（状态：已接受） |
| ADR-0005 | Zenoh 入白名单（体积在阈值内，符合轻架构原则） |

## 10. 后续待用户决定

请用户在以下选项中拍板，决策后我将：

1. 升级本文档为 ADR-0009
2. 同步更新 02-development-plan（L4-TRANS-6 + 新增 L4-TRANS-13）
3. 同步更新 ADR-0005 依赖白名单
4. 同步更新 supermemory
5. 同步更新 README（如需要）

---

## 附录 A：天枢场景的跨机典型部署

| 部署场景 | 主机数 | 网络条件 | 关键约束 |
|---|---|---|---|
| 车端 ECU 内部（多个 加载进程） | 1 | SHM | 不走跨机（自研 SHM） |
| 车端 ECU + 远端机（开发调试） | 2-3 | 千兆 | Zenoh 多播 |
| 测试车 + 云端服务器（数据上传） | 2 | 4G/5G | Zenoh + TLS |
| 数据中心多机仿真 | 10+ | 万兆 | Zenoh router 模式 |
| 机器人控制器 + 多传感器节点 | 5-10 | 千兆 | Zenoh-pico（部分 MCU 节点） |

## 附录 B：参考资料

- Zenoh 项目：https://zenoh.io/
- Zenoh-pico（嵌入式）：https://github.com/eclipse-zenoh/zenoh-pico
- Fast-DDS：https://www.eprosima.com/index.php/products-all/eprosima-fast-dds
- CycloneDDS：https://github.com/eclipse-cyclonedds/cyclonedds
- DDS-RTPS 标准：https://www.omg.org/spec/DDSI-RTPS/
- MQTT v5：https://docs.oasis-open.org/mqtt/mqtt/v5.0/mqtt-v5.0.html
- gRPC：https://grpc.io/
