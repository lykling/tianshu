# ADR-0028：Tianshu Record Format v2 —— 生产级流式记录格式

- **状态**：已接受（取代 v0 dump 与 v1 草案）
- **日期**：2026-09-01
- **决策者**：Pride Leong
- **关联**：[adr/0026](./0026-slice-input-model.md) · [adr/0022](./0022-lineage-v0.md) · [adr/0020](./0020-message-reflection-monitor.md)

---

## 需求清单（评审输入，逐条锁定）

| # | 需求 | 设计响应 |
|---|---|---|
| 1 | 血缘关系记录（离线处理等场景） | 二进制 LineageRecord 引用通道字典；每条消息可选携带 |
| 2 | 严格数据发生时序 | 每条消息 ts_ns（采集时钟）；chunk 内按 ts 排序；chunk 间非重叠 |
| 3 | 压缩 | chunk 级 LZ4/ZSTD/None（header 声明）；解压批处理 |
| 4 | 流式读写 | 写：chunk 攒批追加（不需预知总量）；读：顺序扫或索引随机 |
| 5 | 分片与合并 | 每个文件自包含完整结构（header/dict/chunks/index/footer）；split/merge 操作 |
| 6 | 索引与快速查询 | chunk 级 + 可选 message 级索引；按 (channel, ts/seq) 二分 |
| 7 | 摘要与统计 | Statistics Record：每通道 count/bytes/min-max seq/min-max ts/平均速率 |
| 8 | 回放性能 | 顺序 chunk = 磁盘友好；ts 保序；可选 wallclock 节奏回放；零拷贝 mmap（未压缩 chunk） |
| 9 | 向前向后兼容 | major/minor 版本；record type registry + skip-unknown；flags 管可选段 |
| 10 | 升级空间 | 保留 record type 0x05-0xFF；header 预留字段；TLV 扩展元数据 |
| 11 | schema/descriptor | 通道字典内嵌：格式类型 + 类型名 + schema blob（POD 字段表 / proto FileDescriptorSet / fbs Schema） |

## 文件布局

```
┌───────────────────────────────────────────────────────────────────┐
│ FILE HEADER（固定 128 字节，16 对齐）                              │
│   u64  magic            'TSREC\0\0\2'   （主版本 = 2）           │
│   u16  minor_version    0                                         │
│   u16  header_size      128                                       │
│   u32  flags            bit0=LE, bit1=has_index, bit2=has_stats, │
│                         bit3=compressed, bit4=has_lineage_dict   │
│   u32  header_crc       CRC32(header[0..120])                    │
│   u64  create_ts_ns     录制启动时刻                              │
│   u64  metadata_offset  元数据段偏移（0 = 无）                    │
│   u64  dict_offset      字典段偏移                                │
│   u64  data_offset      首个 chunk 偏移                           │
│   u64  index_offset     索引段偏移（footer 指向，冗余存 header）   │
│   u64  stats_offset     统计段偏移（0 = 无）                      │
│   u32  chunk_count      已写入 chunk 数                           │
│   u32  channel_count    字典通道数                                │
│   u64  total_messages   总消息数（追加中更新于 footer）            │
│   u64  total_bytes      总 payload 字节数                         │
│   u32  compression      0=None, 1=LZ4, 2=ZSTD                    │
│   ... reserved to 128 ...                                        │
├───────────────────────────────────────────────────────────────────┤
│ METADATA RECORD（type 0x04，JSON，可选）                           │
│   { "profile": "vehicle", "host": "orin-01", ... }               │
├───────────────────────────────────────────────────────────────────┤
│ CHANNEL DICTIONARY（type 0x01 × N）                                │
│   u16  channel_id       紧凑 ID（0 起连续分配）                   │
│   u16  name_len + name  通道全名                                  │
│   u16  format           0=POD, 1=Protobuf, 2=FlatBuffers         │
│   u16  type_name_len + type_name                                  │
│   u32  schema_len + schema blob                                   │
│     POD:        ADR-0020 FieldDesc[] 序列化                      │
│     Protobuf:   FileDescriptorSet                                 │
│     FlatBuffers: reflection::Schema 平面缓冲                     │
│   u32  crc32                                                       │
├───────────────────────────────────────────────────────────────────┤
│ DATA CHUNK（type 0x02 × M）                                        │
│   u8   type = 0x02                                                │
│   u32  body_len         压缩后长度（未压缩 = 原始长度）            │
│   u32  uncompressed_len 解压后长度（= body_len when None）         │
│   u8   compression      覆盖文件级（chunk 级可切换）               │
│   u64  body_crc32       CRC32(压缩后 body)                       │
│   u64  ts_first_ns      chunk 内最早时间戳（索引/跳过加速）        │
│   u64  ts_last_ns       chunk 内最晚时间戳                        │
│   ── body（压缩或未压缩）─────────────────────────────────────   │
│   MESSAGE ENTRY × K（ts 升序）                                   │
│     u16  channel_id                                              │
│     u64  seq                                                     │
│     u64  ts_ns               ← 采集时钟（严格时序基准）           │
│     u32  payload_len                                            │
│     ... payload bytes（8 对齐 padding）                          │
│     u32  lineage_blob_len   0 = 无血缘                           │
│     ... LineageRecord bytes（8 对齐）                             │
│   ────────────────────────────────────────────────────────────   │
├───────────────────────────────────────────────────────────────────┤
│ CHUNK INDEX（type 0x03，flags.bit1=1 时存在）                      │
│   u32  entry_count                                                │
│   CHUNK INDEX ENTRY × chunk_count                                 │
│     u64  chunk_offset     文件内偏移                             │
│     u16  channel_bitmap_len  覆盖通道的位图（引用字典 ID）        │
│     ... bitmap bytes                                             │
│     u64  seq_min / seq_max     全通道聚合（粗）                   │
│     u64  ts_first / ts_last    = chunk header 冗余               │
├───────────────────────────────────────────────────────────────────┤
│ MESSAGE INDEX（type 0x05，可选，per-channel）                      │
│   u16  channel_id                                                 │
│   u32  entry_count                                                │
│   MESSAGE INDEX ENTRY × count                                     │
│     u64  seq                                                     │
│     u64  ts_ns                                                    │
│     u64  chunk_offset    ← 定位到 chunk                           │
│     u32  entry_offset    ← chunk body 内偏移                     │
├───────────────────────────────────────────────────────────────────┤
│ STATISTICS（type 0x06，flags.bit2=1 时存在）                       │
│   u32  channel_count                                               │
│   CHANNEL STATS × channel_count                                    │
│     u16  channel_id                                               │
│     u64  message_count                                            │
│     u64  payload_bytes                                            │
│     u64  seq_min / seq_max                                        │
│     u64  ts_min / ts_max                                          │
│     double  avg_rate_hz                                           │
│   u64  total_duration_ns                                          │
├───────────────────────────────────────────────────────────────────┤
│ FILE FOOTER（固定 64 字节）                                        │
│   u64  footer_magic      'TSRECFT\0'                             │
│   u64  index_offset      ← 索引段起始                             │
│   u64  stats_offset      ← 统计段起始                             │
│   u64  dict_offset       ← 字典段起始                             │
│   u32  file_crc32        ← 全文件 CRC（不含 footer 自身）          │
│   u32  footer_crc32      ← CRC32(footer[0..56])                   │
│   u64  total_messages    ← 最终值（冗余于 header）                │
│   u64  total_bytes                                                 │
└───────────────────────────────────────────────────────────────────┘
```

## LineageRecord 二进制序列化

引用通道字典的 compact ID，与消息同 chunk 存储：

```
LineageRecord:
  u16  branch_count
  Per branch:
    u16  root_channel_id     ← 字典查找
    u64  root_seq
    u64  root_seq_end        ← 区间跳（单条时 = root_seq）
    u16  hop_count
    Per hop:
      u16  channel_id
      u64  seq
      u64  seq_end
```

**为什么改变 v1 的"不存血缘"决策**：评审正确指出离线场景（审计、分析、跨工具血缘图查询）需要**不跑图就能读出血缘**。回放时仍可用级联重建（更快），但文件里的血缘是**权威记录**——两者不矛盾，读方自选。

## Record 类型注册表

| type | 含义 | 可跳过 | 版本 |
|---|---|---|---|
| 0x00 | 无效/保留 | — | — |
| 0x01 | 通道字典条目 | ✅ | v2 |
| 0x02 | 数据 chunk | ✅ | v2 |
| 0x03 | chunk 索引 | ✅ | v2 |
| 0x04 | 元数据（JSON） | ✅ | v2 |
| 0x05 | 消息索引 | ✅ | v2 |
| 0x06 | 统计摘要 | ✅ | v2 |
| 0x07 | 字典补丁（merge 时 ID 重映射） | ✅ | v2 |
| 0x08 | 分片链接（指向兄弟文件） | ✅ | 预留 |
| 0x09 | 消息时间戳校正表 | ✅ | 预留 |
| 0x0A-0x7F | 保留 | ✅ | — |
| 0x80-0xFF | 用户自定义 | ✅ | — |

**前向兼容**：所有 record 统一头部 `[u8 type][u32 body_len]`。读方遇未知 type 读 length 跳过。**向后兼容**：v2 读方可读 v1/v0 文件（magic 版本字段区分，走 legacy 解析路径）。

## 分片与合并

**每个文件自包含完整结构**——own header、dictionary、chunks、index、statistics、footer。分片/合并是纯文件级操作：

| 操作 | 语义 |
|---|---|
| `split(ts_range)` | 按时间切：读原文件索引 → 复制 chunk 到新文件 → 各自重写 footer |
| `split(channel_set)` | 按通道切：字典过滤 → chunk 内按 channel bitmap 过滤 entry |
| `merge(files[])` | 字典合并（ID 重映射 + type 0x07 补丁）→ chunk 按 ts 归并 → 重建索引 |
| `append(file)` | merge 的特例：尾部追加 |

## 回放性能设计

| 路径 | 机制 |
|---|---|
| 顺序全量回放 | chunk 顺序读 = 磁盘友好；LZ4 解压 ~3 GB/s（瓶颈在 I/O 不在 CPU） |
| 按通道/时间选择性回放 | 索引二分 → 定位 chunk → mmap（未压缩 chunk 零拷贝） |
| 保时序回放 | entry 内 ts_ns 保序；wallclock pacing（`replay(rate=1.0)`）或 max-speed |
| 血缘重建 vs 文件读取 | 回放选级联重建（快）；分析选文件读取（不跑图）——**双路径** |

## Schema 嵌入（ADR-0020 复用）

通道字典条目的 `schema blob` 字段按 `format` 分发：

| format | blob 内容 | 反序列化消费方 |
|---|---|---|
| POD | `FieldDesc[]` 序列化（name/offset/type/count） | `decode_pod` |
| Protobuf | `FileDescriptorSet`（含依赖） | `DescriptorPool` + `DynamicMessageFactory` |
| FlatBuffers | `reflection::Schema` 平面缓冲 | `flatbuffers::reflection` |

**离线工具打开文件即获全通道 schema**——不需要链接发布方代码、不需要运行时发现，ti-monitor / ti-replay / 第三方分析工具零预配置。

## 压缩策略

| 级别 | 算法 | 理由 |
|---|---|---|
| chunk 级（默认） | LZ4 frame | 解压 3+ GB/s，写路径瓶颈在磁盘不在 CPU；帧格式支持流式 |
| chunk 级（可选） | ZSTD level 3 | 压缩比 ~2× 于 LZ4，解压 ~1 GB/s；适合归档 |
| 无 | None | mmap 直读路径；调试/高频写入 |

选择在 header 声明默认 + chunk 级可逐块覆盖（录制中动态切换）。

## 统计信息

Statistics Record 在文件关闭时写入（footer 前方）。**从索引可直接计算**（不需要扫 payload）：

```
per-channel: count, bytes, seq range, ts range, avg_rate
file-level:  total_messages, total_bytes, duration, channel_list
```

工具（`ti info file.trec`）读 footer → stats_offset → 直接输出，O(1)。

## 与业界格式对比

| 特性 | MCAP | rosbag2 | Tianshu Record v2 |
|---|---|---|---|
| 通道字典 | ✅ | ✅ | ✅ + schema blob |
| 血缘存储 | ❌ | ❌ | ✅ LineageRecord |
| chunk 压缩 | ✅ LZ4/ZSTD | ✅ | ✅ 同 |
| chunk 索引 | ✅ | ❌ | ✅ |
| 消息级索引 | ✅ | ❌ | ✅（可选） |
| 统计摘要 | ✅ | ❌ | ✅ |
| 分片/合并 | 工具级 | 工具级 | 文件格式原生（type 0x07/0x08） |
| 自包含 footer | ✅ | ✅ | ✅ |
| POD schema | ❌（仅消息定义） | ❌ | ✅ ADR-0020 字段表 |
| 回放保时序 | 工具层 | 工具层 | 格式层（ts_ns + pacing API） |
| 前向兼容 | ✅ | 部分 | ✅ type + skip |

## 实现分期

| 阶段 | 内容 |
|---|---|
| v2.0 | header/dict/chunk(CRC)/lineage/footer + 顺序读写 + LZ4 |
| v2.1 | chunk 索引 + 选择性读取 + statistics |
| v2.2 | 消息索引 + split/merge 工具 + ZSTD |
| v2.3 | `ti record` / `ti replay` / `ti info` CLI |

## 风险与缓解

| 风险 | 缓解 |
|---|---|
| 血缘 blob 增大文件 | 每 chunk 按 channel 去重共享 LineageRecord（同一输入派生的多条输出共享引用） |
| chunk 压缩中断崩溃 | chunk 级 CRC 检测；footer 缺失 → 标记不完整但仍可读（索引数据在 header） |
| 大文件索引加载 | chunk 级索引条目少；消息索引按需加载（flags 控制） |
| merge 时 ID 冲突 | type 0x07 字典补丁 record 记录重映射 |
