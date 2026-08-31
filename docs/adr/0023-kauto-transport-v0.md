# ADR-0023：kAuto 自动选路 v0 —— 无 discovery 的双发语义

- **状态**：已接受（已实现）
- **日期**：2026-08-28
- **决策者**：Pride Leong
- **关联**：[adr/0010](./0010-transport-shm-infra.md) · [adr/0015](./0015-discovery-abstraction.md)

---

## 背景

L4-TRANS-21 要求「reader/writer 同进程自动切 INTRA」。原计划依赖 service discovery 的 process_id 比对（L4-SD-4），但 discovery 服务尚未落地，而 DAG 启动器与 DSL 部署现在就需要 `kAuto`。

核心难点是**创建顺序**：同进程的 reader 可能先于 writer 创建。任何"创建时一次性选路"的方案在乱序下都会错配（reader 选了 SHM，本地 writer 却只发 INTRA，或反之）。

## 候选方案

### 方案 1：创建时查注册表，一次性选路

reader 创建时查本进程 INTRA 注册表，有 writer 则 INTRA，否则 SHM；writer 只发所选后端。

**否决**：乱序创建即错配。要么引入"writer 先于 reader"的脆弱契约，要么需要拓扑变更回调（= discovery）。

### 方案 2：intra 为主 + 检测到远端读者后桥接

writer 先只发 INTRA，发现 SHM 侧有读者后起桥接线程转发。

**否决**：桥接启动前的消息丢失；"发现远端读者"本身就需要轮询段状态或 discovery——复杂度回到原点。

### 方案 3：双发写端 + 读端注册表判定（**已选**）

- **写端（kAuto）**：`AutoWriter` 双发——INTRA 注册表扇出（零拷贝）+ SHM broadcast
- **读端（kAuto）**：本进程注册表有**真实 writer** → INTRA reader；否则 → SHM reader

## 决策

**选方案 3**，四点锁定：

1. **顺序无关正确**：reader 先建 → 走 SHM；本地后建的 writer 双发，SHM 路径照样服务它。任何创建顺序下每个 reader 恰好收到一份（reader 只附一个后端，无重复）。
2. **零读者的 SHM broadcast 近零成本**：`ShmChannel::broadcast` 在 `live_slots == 0` 时只递增 seq、循环空转，无 ring 写入。纯同进程链路的实际开销 = 一次原子递增。
3. **真实 writer 与幻影条目区分**：`IntraChannelRegistry` 新增 `register_writer`（create_writer 路径，标记 `published_`）与 `has_writer` 查询；reader 附带创建的条目（`get_or_create_writer`）**不**标记。否则 reader-only 通道会把后续 kAuto reader 误导向 INTRA 而饿死。
4. **已知代价（记录在案）**：每个 kAuto 通道会在 `/dev/shm` 预留一个段（ftruncate 保留虚拟空间；未触碰的 ring 页不占物理内存——内核惰性分配）。纯 intra DAG 付出的是 fd + 映射的一次性成本。

## 演进路径

ADR-0015 的 DiscoveryBackend 落地后：kAuto 切换为**拓扑驱动**——按 (channel × 本地/远端读者集合) 精确选路，撤掉双发与预留段；`AutoWriter` 退役。本 ADR 的测试（顺序无关性、幻影不计数）作为重构守护继续有效。

## 影响范围

- `hybrid_transport.h/.cc`：`AutoWriter` + kAuto 分支
- `intra_backend.h/.cc`：registry 三方法（`register_writer` / `has_writer` / 原 `get_or_create_writer`）
- 功能点：L4-TRANS-21 完成（L4-TRANS-19 的 kAuto 语义随之闭环）
