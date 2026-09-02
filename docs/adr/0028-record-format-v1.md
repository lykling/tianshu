# ADR-0028：Tianshu Record Format v1 —— 分块流式记录文件

- **状态**：已接受（取代 ADR-0026-C 的 v0 原型格式）
- **日期**：2026-09-01
- **决策者**：Pride Leong
- **关联**：[adr/0026](./0026-slice-input-model.md)（切片输入模型）· [adr/0020](./0020-message-reflection-monitor.md)（schema）

---

## 背景

ADR-0026-C 的 v0 RecordFile 是直线 dump（通道名逐条重复、无校验、无索引、无流式追加）。评审指出"太随便"——正确。本 ADR 重新设计 v1 格式，对标 MCAP/rosbag 的成熟做法，适配 TIANSHU 的通道模型与血缘。

## 设计目标

| 目标 | v0 现状 | v1 |
|---|---|---|
| 通道名去重 | 每条重复全名 | 通道字典（name → compact ID） |
| 流式追加 | 必须预知总数 | chunk 化，逐块追加 |
| 完整性校验 | 无 | 每 record CRC32 + 每 chunk CRC32 |
| 随机访问 | 必须全文扫描 | 尾部 chunk 索引（按通道 × seq 区间） |
| schema 嵌入 | 无 | 复用 ADR-0020 字段表（POD/protobuf/fbs） |
| 可扩展 | 版本号但无前向兼容 | record type + 未知类型跳过 |
| 对齐 | 无（packed） | 8 字节对齐（mmap 友好） |

## 文件布局

```
┌─────────────────────────────────────────────────────────────┐
│ FILE HEADER（固定 64 字节，8 对齐）                          │
│   u64  magic          'TSREC\0\0\1'  （含主版本 1）        │
│   u16  minor_version  0                                    │
│   u16  flags          bit0=little_endian, bit1=has_index    │
│   u32  header_crc     CRC32(header[0..60])                 │
│   u32  reserved       0                                    │
│   u64  metadata_len   后跟 metadata JSON 字节数             │
│   u32  chunk_count    文件中 chunk 总数（索引用）            │
│   u32  reserved2      0                                    │
│   u64  create_ts_ns   录制启动时间戳                        │
│   ... padding to 64 ...                                    │
├─────────────────────────────────────────────────────────────┤
│ METADATA（可选 JSON：profile、主机、备注）                   │
├─────────────────────────────────────────────────────────────┤
│ CHANNEL DICTIONARY RECORD × N                               │
│   u8   type = 0x01                                          │
│   u16  channel_id                                           │
│   u16  name_len    + name bytes                             │
│   u16  type_name_len + type_name bytes                      │
│   u16  schema_len  + schema bytes（ADR-0020 blob，可选）     │
│   u32  crc32                                               │
├─────────────────────────────────────────────────────────────┤
│ CHUNK × M（每 chunk 默认 ≤ 1 MiB 或 500 条消息）             │
│   u8   type = 0x02                                          │
│   u32  body_len                                             │
│   u64  body_crc32                                           │
│   ── body（可整体 mmap）─────────────────────────────────   │
│   MESSAGE ENTRY × K                                        │
│     u16  channel_id          ← 字典查找，非全名              │
│     u64  seq                                                │
│     u64  ts_ns               ← 采集时间戳                   │
│     u32  payload_len                                        │
│     ... payload bytes（8 对齐 padding）                     │
│   ────────────────────────────────────────────────────   │
├─────────────────────────────────────────────────────────────┤
│ CHUNK INDEX（可选，flags.bit1 = 1 时存在）                   │
│   u8   type = 0x03                                          │
│   u32  entry_count                                          │
│   INDEX ENTRY × chunk_count                                 │
│     u64  chunk_offset      ← 文件内偏移                     │
│     u16  channel_id        ← 该 chunk 包含的通道            │
│     u64  seq_lo / seq_hi                                    │
│     u64  ts_lo  / ts_hi                                     │
├─────────────────────────────────────────────────────────────┤
│ FILE FOOTER（固定 32 字节）                                  │
│   u64  footer_magic    'TSRECFT\0'                         │
│   u64  index_offset    ← 索引 record 起始偏移               │
│   u64  channel_dict_offset ← 字典起始偏移                   │
│   u32  file_crc32      ← 全文件 CRC（不含自身）              │
│   u32  footer_crc32    ← CRC32(footer[0..28])               │
└─────────────────────────────────────────────────────────────┘
```

## Record 类型注册表

| type | 含义 | 可跳过 |
|---|---|---|
| 0x00 | 保留（无效） | — |
| 0x01 | 通道字典条目 | 是 |
| 0x02 | 数据 chunk | 是 |
| 0x03 | chunk 索引 | 是 |
| 0x04 | 元数据块（文件级追加） | 是 |
| 0x05-0xFF | 保留给扩展 | **是**（读 length 跳过） |

**前向兼容机制**：未知 type 的 record，读取方按 `record header 中的 length` 跳过。所有 record 统一头部：`[u8 type][u32 body_len]`，读方只解析已知类型。

## 关键设计决策

### 1. 通道字典在前、消息引用 compact ID

v0 每条消息重复通道全名（`"avp/radar/front"` = 16 字节 × 每条消息）。v1：字典条目一次，消息内 2 字节 `channel_id`。30 分钟 200Hz IMU 录制节省 ~100 MB。

字典条目**内嵌 ADR-0020 的 schema blob**——离线工具打开文件即知每条通道的消息类型和字段布局（不需要链接发布方代码），ti-monitor 离线解码零预配置。

### 2. Chunk 化 + 8 字节对齐

- 每 chunk 默认攒 ≤ 1 MiB 或 500 条消息再写盘——**顺序 I/O，不是逐消息 syscall**
- chunk body 内所有字段 8 字节对齐——`mmap` 后可直接 `reinterpret_cast` 消费（零拷贝读取路径）
- 写方 `flush_chunk()` 可显式触发（录制结束/定时器到）

### 3. 三级 CRC

| 级别 | 覆盖范围 | 检测 |
|---|---|---|
| entry | 不设（性能） | — |
| chunk | body 全部 | 传输/存储损坏定位到块 |
| file | 全文件 | 整体完整性 |

### 4. 尾部索引 = 随机访问

每个 chunk 记录 `(offset, channel_id, seq_range, ts_range)`。离线切片查询（"给我 imu 通道 seq 10000-20000"）先查索引定位 chunk，再 mmap 该 chunk 精读——不用全文扫描。与 ADR-0026 的切片 API 对齐：**同一查询接口，内存环还是 record 文件对调用方透明**。

### 5. 血缘不进文件

维持 v0 决策：**回放时血缘由级联重建，不从文件读取**。文件中的 lineage 字段仅存 describe 字符串，供工具展示。理由：血缘语义由图的执行模型保证（ADR-0022/0025/0027），从文件读会引入格式变更/手工编辑的一致性风险。

### 6. Metadata JSON

文件级元数据（profile 名、主机名、录制命令行、备注）以 JSON 存储在 header 后。结构自由，读方忽略未知键。

## 与 MCAP 的对比

| | MCAP | Tianshu Record v1 |
|---|---|---|
| 通道字典 | ✅ | ✅ + 内嵌 schema blob |
| chunk 化 | ✅（可压缩） | ✅（v1 不压缩，预留 flags） |
| CRC | ✅ per chunk + message | ✅ per chunk + file（省 entry 级，性能） |
| 索引 | ✅ chunk + message 级 | ✅ chunk 级（够用，message 级预留） |
| schema | protobuf/fbs | ADR-0020 全格式（含 POD 字段表） |
| 压缩 | lz4/zstd | 预留 flags（v1 不做） |
| 血缘 | 无 | describe 字符串（回放不用） |

## 实现

- `tianshu/include/tianshu/dsl/record.h` — RecordWriter（流式追加 + chunk 攒批）/ RecordReader（mmap 索引 + 随机访问）
- `FlowRuntime::record_to` → 改为 RecordWriter 逐通道逐条追加（字典自动注册）
- `FlowRuntime::replay_from` → 改为 RecordReader 按通道过滤回放
- 兼容：v0 格式文件可读（magic 区分），写只出 v1

## 风险与缓解

| 风险 | 缓解 |
|---|---|
| chunk 攒批期间进程崩溃 | 每 chunk CRC 可检测；尾部无 footer → 文件不完整标记 |
| 大文件索引加载慢 | chunk 级索引条目少（每 ~1MB 一条），内存占用可忽略 |
| schema blob 与实际消息不匹配 | CRC + 回放时 ADR-0020 decode 的防御性解析（越界跳过） |
