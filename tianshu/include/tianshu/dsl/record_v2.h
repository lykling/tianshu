// Copyright 2026 Pride Leong.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Tianshu Record Format v2 (ADR-0028): chunked, compressed, indexed,
// lineage-carrying stream file.
//
//   FILE HEADER (128B, fixed)
//   METADATA (JSON, optional)
//   CHANNEL DICTIONARY (type 0x01, with schema blobs)
//   DATA CHUNKS (type 0x02, LZ4/ZSTD/None, CRC'd, timestamp-ordered)
//   CHUNK INDEX (type 0x03, random access)
//   STATISTICS (type 0x06, per-channel summary)
//   FILE FOOTER (64B, fixed)
//
// All integers little-endian. Message entries 8-byte aligned.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tianshu/core/lineage.h"

namespace tianshu::dsl::record {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

inline constexpr std::uint64_t kMagicV2 = 0x54535245'430002ULL;        // 'TSREC\0\0\2'
inline constexpr std::uint64_t kFooterMagic = 0x54535245'43546000ULL;  // 'TSRECFT\0'
inline constexpr std::size_t kHeaderSize = 128;
inline constexpr std::size_t kFooterSize = 64;
inline constexpr std::size_t kMaxChunkMessages = 500;
inline constexpr std::size_t kMaxChunkBytes = std::size_t{1} << 20;  // 1 MiB

enum class Compression : std::uint8_t {
  kNone = 0,
  kLz4 = 1,
  kZstd = 2,
};

// Record types (forward-compat: unknown types are skipped by readers).
enum class RecordType : std::uint8_t {
  kReserved = 0x00,
  kChannelDict = 0x01,
  kDataChunk = 0x02,
  kChunkIndex = 0x03,
  kMetadata = 0x04,
  kMessageIndex = 0x05,
  kStatistics = 0x06,
  kDictPatch = 0x07,
  kShardLink = 0x08,
  kTsCorrection = 0x09,
};

// ---------------------------------------------------------------------------
// Public data types
// ---------------------------------------------------------------------------

struct ChannelEntry {
  std::uint16_t id{0};
  std::string name;
  std::uint8_t format{0};  // 0=POD, 1=Protobuf, 2=FlatBuffers
  std::string type_name;
  std::vector<std::uint8_t> schema_blob;
  // Filled by reader from the statistics section.
  std::uint64_t message_count{0};
  std::uint64_t payload_bytes{0};
  std::uint64_t seq_min{0};
  std::uint64_t seq_max{0};
  std::uint64_t ts_min{0};
  std::uint64_t ts_max{0};
  double avg_rate_hz{0.0};
};

struct RecordedMessageV2 {
  std::uint16_t channel_id{0};
  std::uint64_t seq{0};
  std::uint64_t ts_ns{0};
  std::vector<std::uint8_t> payload;
  std::optional<core::Lineage> lineage;
};

struct ChunkIndexEntry {
  std::uint64_t chunk_offset{0};
  std::vector<std::uint16_t> channel_ids;
  std::uint64_t ts_first{0};
  std::uint64_t ts_last{0};
};

struct RecordStats {
  std::uint64_t total_messages{0};
  std::uint64_t total_bytes{0};
  std::uint64_t duration_ns{0};
  std::vector<ChannelEntry> channels;
};

// ---------------------------------------------------------------------------
// LineageRecord binary serialization (references channel dictionary IDs)
// ---------------------------------------------------------------------------

[[nodiscard]] std::vector<std::uint8_t> serialize_lineage(
    const core::Lineage& lin, const std::function<std::uint16_t(const std::string&)>& id_for);

[[nodiscard]] core::Lineage deserialize_lineage(
    const std::vector<std::uint8_t>& blob,
    const std::function<const std::string&(std::uint16_t)>& name_for);

// ---------------------------------------------------------------------------
// CRC32 (IEEE 802.3, table-based)
// ---------------------------------------------------------------------------

[[nodiscard]] std::uint32_t crc32(const void* data, std::size_t size,
                                  std::uint32_t seed = 0xFFFFFFFFU);

// ---------------------------------------------------------------------------
// RecordWriter: streaming append with chunk batching
// ---------------------------------------------------------------------------

class RecordWriter {
 public:
  explicit RecordWriter(const std::string& path, Compression compression = Compression::kLz4);
  ~RecordWriter();  // calls finish() if not already called

  RecordWriter(const RecordWriter&) = delete;
  RecordWriter& operator=(const RecordWriter&) = delete;

  // Registers a channel and returns its compact ID (sequential from 0).
  [[nodiscard]] std::uint16_t add_channel(const std::string& name, std::uint8_t format,
                                          const std::string& type_name,
                                          const std::vector<std::uint8_t>& schema_blob = {});

  // Appends a message into the current chunk (batched; flush on size/count).
  void append(std::uint16_t channel_id, std::uint64_t seq, std::uint64_t ts_ns, const void* data,
              std::size_t size, const core::Lineage* lineage = nullptr);

  // Forces the current chunk to disk (end of recording, periodic flush).
  void flush_chunk();

  // Writes index, statistics, footer. Returns false on I/O failure.
  bool finish();

  [[nodiscard]] std::uint64_t message_count() const;
  [[nodiscard]] const std::vector<ChannelEntry>& channels() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// RecordReader: sequential + indexed random access
// ---------------------------------------------------------------------------

class RecordReader {
 public:
  // Opens and validates (header CRC, footer CRC). Returns nullopt on failure.
  [[nodiscard]] static std::optional<RecordReader> open(const std::string& path);

  RecordReader() = default;
  ~RecordReader() = default;
  RecordReader(RecordReader&&) = default;
  RecordReader& operator=(RecordReader&&) = default;

  // Dictionary access.
  [[nodiscard]] const std::vector<ChannelEntry>& channels() const;
  [[nodiscard]] const ChannelEntry* find_channel(const std::string& name) const;
  [[nodiscard]] const ChannelEntry* find_channel(std::uint16_t id) const;

  // Sequential iteration (timestamp order).
  bool next(RecordedMessageV2* msg);
  void seek(std::uint64_t ts_ns);

  // Selective read via chunk index: all messages on `channel_id` in
  // [ts_lo, ts_hi] (inclusive).
  [[nodiscard]] std::vector<RecordedMessageV2> read_range(std::uint16_t channel_id,
                                                          std::uint64_t ts_lo, std::uint64_t ts_hi);

  // Statistics (O(1) from the statistics section; empty if absent).
  [[nodiscard]] const RecordStats& stats() const;

  // Format version (2 for this format).
  [[nodiscard]] std::uint16_t major_version() const;

  // Chunk index (empty if the file has no index section).
  [[nodiscard]] const std::vector<ChunkIndexEntry>& chunk_index() const;

  // Merge: concatenates channels (re-IDing) and messages (ts-ordered).
  [[nodiscard]] static bool merge(const std::vector<std::string>& inputs,
                                  const std::string& output);

  // Split: writes messages in [ts_lo, ts_hi] to output (inclusive).
  [[nodiscard]] static bool split_by_time(const std::string& input, const std::string& output,
                                          std::uint64_t ts_lo, std::uint64_t ts_hi);

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace tianshu::dsl::record
