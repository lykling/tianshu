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

#include "tianshu/dsl/record_v2.h"

#include <lz4.h>
#include <zstd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "tianshu/core/lineage.h"

namespace tianshu::dsl::record {
namespace {

// --- binary I/O helpers (little-endian) ------------------------------------

void w_u8(std::vector<std::uint8_t>* out, std::uint8_t v) { out->push_back(v); }

void w_u16(std::vector<std::uint8_t>* out, std::uint16_t v) {
  for (int i = 0; i < 2; ++i) {
    out->push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
  }
}

void w_u32(std::vector<std::uint8_t>* out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out->push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
  }
}

void w_u64(std::vector<std::uint8_t>* out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out->push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
  }
}

void w_f64(std::vector<std::uint8_t>* out, double v) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &v, 8);
  w_u64(out, bits);
}

void w_bytes(std::vector<std::uint8_t>* out, const void* data, std::size_t n) {
  const auto* b = static_cast<const std::uint8_t*>(data);
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  out->insert(out->end(), b, b + n);
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
}

void w_pad8(std::vector<std::uint8_t>* out) {
  while (out->size() % 8 != 0) {
    out->push_back(0);
  }
}

struct Cursor {
  const std::vector<std::uint8_t>* buf;
  std::size_t pos{0};

  [[nodiscard]] bool ok(std::size_t n) const { return pos + n <= buf->size(); }

  std::uint8_t r_u8() { return ok(1) ? (*buf)[pos++] : 0; }

  std::uint16_t r_u16() {
    if (!ok(2)) {
      return 0;
    }
    std::uint16_t v = 0;
    for (int i = 0; i < 2; ++i) {
      v = static_cast<std::uint16_t>(
          v | (static_cast<std::uint16_t>((*buf)[pos + static_cast<std::size_t>(i)]) << (8 * i)));
    }
    pos += 2;
    return v;
  }

  std::uint32_t r_u32() {
    if (!ok(4)) {
      return 0;
    }
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      v |= static_cast<std::uint32_t>((*buf)[pos + static_cast<std::size_t>(i)]) << (8 * i);
    }
    pos += 4;
    return v;
  }

  std::uint64_t r_u64() {
    if (!ok(8)) {
      return 0;
    }
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
      v |= static_cast<std::uint64_t>((*buf)[pos + static_cast<std::size_t>(i)]) << (8 * i);
    }
    pos += 8;
    return v;
  }

  double r_f64() {
    const std::uint64_t bits = r_u64();
    double v = 0.0;
    std::memcpy(&v, &bits, 8);
    return v;
  }

  std::vector<std::uint8_t> r_bytes(std::size_t n) {
    if (!ok(n)) {
      return {};
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    std::vector<std::uint8_t> out(buf->begin() + static_cast<std::ptrdiff_t>(pos),
                                  buf->begin() + static_cast<std::ptrdiff_t>(pos + n));
    pos += n;
    return out;
  }
};

// --- compression wrappers ----------------------------------------------------

std::vector<std::uint8_t> compress_bytes(const std::vector<std::uint8_t>& input,
                                         Compression method) {
  if (method == Compression::kNone || input.empty()) {
    return input;
  }
  if (method == Compression::kLz4) {
    const int bound = LZ4_compressBound(static_cast<int>(input.size()));
    std::vector<std::uint8_t> out(static_cast<std::size_t>(bound));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const int written = LZ4_compress_default(reinterpret_cast<const char*>(input.data()),
                                             reinterpret_cast<char*>(out.data()),
                                             static_cast<int>(input.size()), bound);
    if (written <= 0) {
      return input;
    }
    out.resize(static_cast<std::size_t>(written));
    return out;
  }
  // ZSTD
  const std::size_t bound = ZSTD_compressBound(input.size());
  std::vector<std::uint8_t> out(bound);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const std::size_t written = ZSTD_compress(out.data(), bound, input.data(), input.size(), 3);
  if (ZSTD_isError(written) != 0U) {
    return input;
  }
  out.resize(written);
  return out;
}

std::vector<std::uint8_t> decompress_bytes(const std::vector<std::uint8_t>& input,
                                           std::size_t uncompressed_len, Compression method) {
  if (method == Compression::kNone || input.size() == uncompressed_len) {
    return input;
  }
  if (method == Compression::kLz4) {
    std::vector<std::uint8_t> out(uncompressed_len);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const int decoded = LZ4_decompress_safe(
        reinterpret_cast<const char*>(input.data()), reinterpret_cast<char*>(out.data()),
        static_cast<int>(input.size()), static_cast<int>(uncompressed_len));
    if (decoded < 0) {
      return {};
    }
    out.resize(static_cast<std::size_t>(decoded));
    return out;
  }
  // ZSTD
  std::vector<std::uint8_t> out(uncompressed_len);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const std::size_t decoded =
      ZSTD_decompress(out.data(), uncompressed_len, input.data(), input.size());
  if (ZSTD_isError(decoded) != 0U) {
    return {};
  }
  out.resize(decoded);
  return out;
}

// --- chunk body entry serialization -------------------------------------------

struct PendingMessage {
  std::uint16_t channel_id;
  std::uint64_t seq;
  std::uint64_t ts_ns;
  std::vector<std::uint8_t> payload;
  std::optional<std::vector<std::uint8_t>> lineage_blob;
};

void encode_message(std::vector<std::uint8_t>* body, const PendingMessage& msg) {
  w_u16(body, msg.channel_id);
  w_u64(body, msg.seq);
  w_u64(body, msg.ts_ns);
  w_u32(body, static_cast<std::uint32_t>(msg.payload.size()));
  w_bytes(body, msg.payload.data(), msg.payload.size());
  w_pad8(body);
  w_u32(body,
        static_cast<std::uint32_t>(msg.lineage_blob.has_value() ? (*msg.lineage_blob).size() : 0));
  if (msg.lineage_blob.has_value()) {
    w_bytes(body, (*msg.lineage_blob).data(), (*msg.lineage_blob).size());
    w_pad8(body);
  }
}

bool decode_message(Cursor* cur, PendingMessage* msg) {
  msg->channel_id = cur->r_u16();
  msg->seq = cur->r_u64();
  msg->ts_ns = cur->r_u64();
  const std::uint32_t plen = cur->r_u32();
  if (plen > 64 * 1024 * 1024) {
    return false;
  }
  msg->payload = cur->r_bytes(plen);
  // Align to 8 based on ABSOLUTE position (matches w_pad8 in the writer).
  cur->pos = (cur->pos + 7) & ~static_cast<std::size_t>(7);
  const std::uint32_t llen = cur->r_u32();
  if (llen > 0) {
    msg->lineage_blob = cur->r_bytes(llen);
    cur->pos = (cur->pos + 7) & ~static_cast<std::size_t>(7);
  }
  return true;
}

}  // namespace

// --- CRC32 (public) ------------------------------------------------------------

std::uint32_t crc32(const void* data, std::size_t size, std::uint32_t seed) {
  static std::uint32_t table[256] = {};
  static bool initialized = false;
  if (!initialized) {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int j = 0; j < 8; ++j) {
        c = (c & 1) != 0 ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
      }
      table[i] = c;
    }
    initialized = true;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::uint32_t crc = seed;
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  for (std::size_t i = 0; i < size; ++i) {
    crc = table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  return crc ^ 0xFFFFFFFFU;
}

// --- LineageRecord serialization (public) --------------------------------------

std::vector<std::uint8_t> serialize_lineage(
    const core::Lineage& lin, const std::function<std::uint16_t(const std::string&)>& id_for) {
  std::vector<std::uint8_t> out;
  w_u16(&out, static_cast<std::uint16_t>(lin.branches().size()));
  for (const auto& branch : lin.branches()) {
    w_u16(&out, id_for(branch.root.channel));
    w_u64(&out, branch.root.seq);
    w_u64(&out, branch.root.seq_end);
    w_u16(&out, static_cast<std::uint16_t>(branch.hops.size()));
    for (const auto& hop : branch.hops) {
      w_u16(&out, id_for(hop.channel));
      w_u64(&out, hop.seq);
      w_u64(&out, hop.seq_end);
    }
  }
  return out;
}

core::Lineage deserialize_lineage(
    const std::vector<std::uint8_t>& blob,
    const std::function<const std::string&(std::uint16_t)>& name_for) {
  Cursor cur{.buf = &blob, .pos = 0};
  core::Lineage lin;
  const std::uint16_t branch_count = cur.r_u16();
  for (std::uint16_t b = 0; b < branch_count; ++b) {
    const std::uint16_t root_id = cur.r_u16();
    const std::uint64_t root_seq = cur.r_u64();
    const std::uint64_t root_end = cur.r_u64();
    core::Lineage branch_lin =
        root_end > root_seq ? core::Lineage::rooted_range(name_for(root_id), root_seq, root_end)
                            : core::Lineage::rooted(name_for(root_id), root_seq);
    const std::uint16_t hop_count = cur.r_u16();
    for (std::uint16_t h = 0; h < hop_count; ++h) {
      const std::uint16_t hop_id = cur.r_u16();
      const std::uint64_t hop_seq = cur.r_u64();
      const std::uint64_t hop_end = cur.r_u64();
      if (hop_end > hop_seq) {
        branch_lin.add_range_hop(name_for(hop_id), hop_seq, hop_end);
      } else {
        branch_lin.add_hop({.channel = name_for(hop_id), .seq = hop_seq, .seq_end = hop_seq});
      }
    }
    lin.merge(branch_lin);
  }
  return lin;
}

// --- RecordWriter::Impl --------------------------------------------------------

struct RecordWriter::Impl {
  FILE* file{nullptr};
  Compression compression{Compression::kLz4};
  std::string path;

  std::vector<ChannelEntry> channels;
  std::unordered_map<std::string, std::uint16_t> name_to_id;

  std::vector<PendingMessage> pending;  // current chunk batch
  std::size_t pending_bytes{0};
  std::uint64_t total_messages{0};
  std::uint64_t total_payload_bytes{0};
  std::uint64_t first_ts{0};
  std::uint64_t last_ts{0};

  // Per-channel stats
  std::unordered_map<std::uint16_t, ChannelEntry> live_stats;

  // Chunk offsets for index
  std::vector<std::uint64_t> chunk_offsets;
  bool finished{false};
  bool header_written{false};
};

RecordWriter::RecordWriter(const std::string& path, Compression compression)
    : impl_(std::make_unique<Impl>()) {
  impl_->file = std::fopen(path.c_str(), "wb");  // NOLINT(concurrency-mt-unsafe)
  impl_->path = path;
  impl_->compression = compression;
  if (impl_->file != nullptr) {
    // Placeholder header (rewritten by finish()); exact kHeaderSize bytes
    // so subsequent data starts at offset 128.
    const std::vector<std::uint8_t> header(kHeaderSize, 0);
    static_cast<void>(std::fwrite(header.data(), 1, header.size(), impl_->file));
    impl_->header_written = true;
  }
}

RecordWriter::~RecordWriter() {
  if (impl_ != nullptr && !impl_->finished) {
    static_cast<void>(finish());
  }
}

std::uint16_t RecordWriter::add_channel(const std::string& name, std::uint8_t format,
                                        const std::string& type_name,
                                        const std::vector<std::uint8_t>& schema_blob) {
  const auto it = impl_->name_to_id.find(name);
  if (it != impl_->name_to_id.end()) {
    return it->second;
  }
  const auto id = static_cast<std::uint16_t>(impl_->channels.size());
  const ChannelEntry entry{
      .id = id, .name = name, .format = format, .type_name = type_name, .schema_blob = schema_blob};
  impl_->channels.push_back(entry);
  impl_->name_to_id[name] = id;
  impl_->live_stats[id] = entry;
  return id;
}

void RecordWriter::append(std::uint16_t channel_id, std::uint64_t seq, std::uint64_t ts_ns,
                          const void* data, std::size_t size, const core::Lineage* lineage) {
  if (impl_->file == nullptr) {
    return;
  }
  PendingMessage msg;
  msg.channel_id = channel_id;
  msg.seq = seq;
  msg.ts_ns = ts_ns;
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  msg.payload.assign(static_cast<const std::uint8_t*>(data),
                     static_cast<const std::uint8_t*>(data) + size);
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  if (lineage != nullptr && !lineage->empty()) {
    msg.lineage_blob = serialize_lineage(*lineage, [this](const std::string& ch) {
      const auto it = impl_->name_to_id.find(ch);
      return it != impl_->name_to_id.end() ? it->second : static_cast<std::uint16_t>(0);
    });
  }
  impl_->pending_bytes += size + 32;  // overhead estimate
  impl_->pending.push_back(std::move(msg));

  // Update stats.
  auto& st = impl_->live_stats[channel_id];
  if (st.message_count == 0) {
    st.seq_min = st.seq_max = seq;
    st.ts_min = st.ts_max = ts_ns;
  } else {
    st.seq_min = std::min(st.seq_min, seq);
    st.seq_max = std::max(st.seq_max, seq);
    st.ts_min = std::min(st.ts_min, ts_ns);
    st.ts_max = std::max(st.ts_max, ts_ns);
  }
  st.message_count++;
  st.payload_bytes += size;
  impl_->total_messages++;
  impl_->total_payload_bytes += size;
  if (impl_->total_messages == 1) {
    impl_->first_ts = ts_ns;
  }
  impl_->last_ts = ts_ns;

  if (impl_->pending.size() >= kMaxChunkMessages || impl_->pending_bytes >= kMaxChunkBytes) {
    flush_chunk();
  }
}

void RecordWriter::flush_chunk() {
  if (impl_->file == nullptr || impl_->pending.empty()) {
    return;
  }
  // Sort by timestamp within the chunk (strict temporal ordering).
  std::sort(impl_->pending.begin(), impl_->pending.end(),
            [](const PendingMessage& a, const PendingMessage& b) { return a.ts_ns < b.ts_ns; });

  // Build chunk body.
  std::vector<std::uint8_t> body;
  for (const auto& msg : impl_->pending) {
    encode_message(&body, msg);
  }
  const std::uint64_t ts_first = impl_->pending.front().ts_ns;
  const std::uint64_t ts_last = impl_->pending.back().ts_ns;

  // Compress.
  const std::vector<std::uint8_t> compressed = compress_bytes(body, impl_->compression);

  // Record the chunk offset.
  const auto chunk_offset = static_cast<std::uint64_t>(std::ftell(impl_->file));
  impl_->chunk_offsets.push_back(chunk_offset);

  // Write chunk record.
  std::vector<std::uint8_t> rec;
  w_u8(&rec, static_cast<std::uint8_t>(RecordType::kDataChunk));
  w_u32(&rec, static_cast<std::uint32_t>(compressed.size()));
  w_u32(&rec, static_cast<std::uint32_t>(body.size()));
  w_u8(&rec, static_cast<std::uint8_t>(impl_->compression));
  const std::uint32_t body_crc = crc32(compressed.data(), compressed.size());
  w_u32(&rec, body_crc);
  w_u64(&rec, ts_first);
  w_u64(&rec, ts_last);
  w_bytes(&rec, compressed.data(), compressed.size());
  static_cast<void>(std::fwrite(rec.data(), 1, rec.size(), impl_->file));

  impl_->pending.clear();
  impl_->pending_bytes = 0;
}

// NOLINTNEXTLINE(readability-function-size): monolithic by design (single write pass)
bool RecordWriter::finish() {
  if (impl_->file == nullptr || impl_->finished) {
    return false;
  }
  impl_->finished = true;

  flush_chunk();

  // --- Channel dictionary ---
  const auto dict_offset = static_cast<std::uint64_t>(std::ftell(impl_->file));
  for (const auto& ch : impl_->channels) {
    std::vector<std::uint8_t> rec;
    w_u8(&rec, static_cast<std::uint8_t>(RecordType::kChannelDict));
    w_u16(&rec, ch.id);
    w_u16(&rec, static_cast<std::uint16_t>(ch.name.size()));
    w_bytes(&rec, ch.name.data(), ch.name.size());
    w_u8(&rec, ch.format);
    w_u16(&rec, static_cast<std::uint16_t>(ch.type_name.size()));
    w_bytes(&rec, ch.type_name.data(), ch.type_name.size());
    w_u32(&rec, static_cast<std::uint32_t>(ch.schema_blob.size()));
    if (!ch.schema_blob.empty()) {
      w_bytes(&rec, ch.schema_blob.data(), ch.schema_blob.size());
    }
    const auto crc = crc32(
        rec.data() + 1, rec.size() - 1);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    w_u32(&rec, crc);
    static_cast<void>(std::fwrite(rec.data(), 1, rec.size(), impl_->file));
  }

  // --- Chunk index ---
  const auto index_offset = static_cast<std::uint64_t>(std::ftell(impl_->file));
  {
    std::vector<std::uint8_t> rec;
    w_u8(&rec, static_cast<std::uint8_t>(RecordType::kChunkIndex));
    w_u32(&rec, static_cast<std::uint32_t>(impl_->chunk_offsets.size()));
    for (std::size_t i = 0; i < impl_->chunk_offsets.size(); ++i) {
      w_u64(&rec, impl_->chunk_offsets[i]);
      // We don't track per-chunk channel lists in v2.0; use a simple bitmap.
      const auto bitmap_words = static_cast<std::uint16_t>((impl_->channels.size() + 15) / 16);
      w_u16(&rec, bitmap_words);
      // TODO(perf): track actual channels per chunk; write all for now.
      std::vector<std::uint8_t> bitmap(static_cast<std::size_t>(bitmap_words) * 2, 0xFF);
      w_bytes(&rec, bitmap.data(), bitmap.size());
      w_u64(&rec, 0);  // seq_min/max placeholder
      w_u64(&rec, 0);
      w_u64(&rec, 0);  // ts placeholder (chunk header has it)
      w_u64(&rec, 0);
    }
    static_cast<void>(std::fwrite(rec.data(), 1, rec.size(), impl_->file));
  }

  // --- Statistics ---
  const auto stats_offset = static_cast<std::uint64_t>(std::ftell(impl_->file));
  {
    std::vector<std::uint8_t> rec;
    w_u8(&rec, static_cast<std::uint8_t>(RecordType::kStatistics));
    w_u32(&rec, static_cast<std::uint32_t>(impl_->live_stats.size()));
    for (const auto& [id, st] : impl_->live_stats) {
      w_u16(&rec, id);
      w_u64(&rec, st.message_count);
      w_u64(&rec, st.payload_bytes);
      w_u64(&rec, st.seq_min);
      w_u64(&rec, st.seq_max);
      w_u64(&rec, st.ts_min);
      w_u64(&rec, st.ts_max);
      const double duration_s =
          st.ts_max > st.ts_min ? static_cast<double>(st.ts_max - st.ts_min) / 1e9 : 0.0;
      const double rate =
          duration_s > 0.0 ? static_cast<double>(st.message_count) / duration_s : 0.0;
      w_f64(&rec, rate);
    }
    w_u64(&rec, impl_->last_ts > impl_->first_ts ? impl_->last_ts - impl_->first_ts : 0);
    static_cast<void>(std::fwrite(rec.data(), 1, rec.size(), impl_->file));
  }

  // --- Footer ---
  {
    std::vector<std::uint8_t> footer;
    w_u64(&footer, kFooterMagic);
    w_u64(&footer, index_offset);
    w_u64(&footer, stats_offset);
    w_u64(&footer, dict_offset);
    w_u32(&footer, 0);  // file CRC (full-file, deferred to v2.1)
    const std::uint32_t footer_crc = crc32(footer.data(), footer.size());
    w_u32(&footer, footer_crc);
    w_u64(&footer, impl_->total_messages);
    w_u64(&footer, impl_->total_payload_bytes);
    footer.resize(kFooterSize, 0);
    static_cast<void>(std::fwrite(footer.data(), 1, footer.size(), impl_->file));
  }

  // --- Rewrite header with real offsets (build from EMPTY, pad to size) ---
  std::vector<std::uint8_t> header;
  w_u64(&header, kMagicV2);
  w_u16(&header, 0);  // minor version
  w_u16(&header, static_cast<std::uint16_t>(kHeaderSize));
  w_u16(&header, 0x0001 | 0x0002 | 0x0004);  // flags: LE + has_index + has_stats
  w_u16(&header, 0);                         // reserved
  w_u32(&header, 0);                         // header CRC placeholder (patched below)
  w_u64(&header, impl_->first_ts);
  w_u64(&header, 0);  // metadata_offset (no metadata in v2.0)
  w_u64(&header, dict_offset);
  w_u64(&header, index_offset);
  w_u64(&header, stats_offset);
  w_u32(&header, static_cast<std::uint32_t>(impl_->chunk_offsets.size()));
  w_u32(&header, static_cast<std::uint32_t>(impl_->channels.size()));
  w_u64(&header, impl_->total_messages);
  w_u64(&header, impl_->total_payload_bytes);
  w_u32(&header, static_cast<std::uint32_t>(impl_->compression));
  header.resize(kHeaderSize, 0);  // pad to exact 128

  // Compute header CRC over bytes [0..123], patch at [124..127]
  const std::uint32_t hdr_crc = crc32(header.data(), kHeaderSize - 4);
  header[124] = static_cast<std::uint8_t>(hdr_crc & 0xFF);
  header[125] = static_cast<std::uint8_t>((hdr_crc >> 8) & 0xFF);
  header[126] = static_cast<std::uint8_t>((hdr_crc >> 16) & 0xFF);
  header[127] = static_cast<std::uint8_t>((hdr_crc >> 24) & 0xFF);

  static_cast<void>(std::fseek(impl_->file, 0, SEEK_SET));
  static_cast<void>(std::fwrite(header.data(), 1, header.size(), impl_->file));
  static_cast<void>(std::fflush(impl_->file));
  static_cast<void>(std::fclose(impl_->file));
  impl_->file = nullptr;
  return true;
}

std::uint64_t RecordWriter::message_count() const { return impl_->total_messages; }

const std::vector<ChannelEntry>& RecordWriter::channels() const { return impl_->channels; }

// --- RecordReader::Impl --------------------------------------------------------

struct RecordReader::Impl {
  std::vector<std::uint8_t> file_data;
  std::uint16_t major_ver{2};

  std::vector<ChannelEntry> channel_list;
  std::unordered_map<std::string, std::uint16_t> name_to_id;
  std::unordered_map<std::uint16_t, std::string> id_to_name;
  RecordStats stats;
  std::vector<ChunkIndexEntry> chunks;

  // Sequential read state.
  std::size_t data_pos{0};  // offset in file_data of next unread data
  std::vector<PendingMessage> current_chunk;
  std::size_t chunk_msg_pos{0};
  bool loaded{false};

  [[nodiscard]] const std::string& name_for(std::uint16_t id) const {
    static const std::string UNKNOWN = "?";
    const auto it = id_to_name.find(id);
    return it != id_to_name.end() ? it->second : UNKNOWN;
  }

  [[nodiscard]] std::uint16_t id_for(const std::string& name) const {
    const auto it = name_to_id.find(name);
    return it != name_to_id.end() ? it->second : 0;
  }

  void load_chunk_at(std::size_t offset) {
    current_chunk.clear();
    chunk_msg_pos = 0;
    if (offset >= file_data.size()) {
      return;
    }
    Cursor cur{.buf = &file_data, .pos = offset};
    if (cur.r_u8() != static_cast<std::uint8_t>(RecordType::kDataChunk)) {
      return;
    }
    const std::uint32_t comp_len = cur.r_u32();
    const std::uint32_t uncomp_len = cur.r_u32();
    const auto method = static_cast<Compression>(cur.r_u8());
    static_cast<void>(cur.r_u32());  // CRC (validated at file level in v2.0)
    static_cast<void>(cur.r_u64());  // ts_first
    static_cast<void>(cur.r_u64());  // ts_last
    const std::vector<std::uint8_t> compressed = cur.r_bytes(comp_len);
    const std::vector<std::uint8_t> body = decompress_bytes(compressed, uncomp_len, method);

    Cursor body_cur{.buf = &body, .pos = 0};
    while (body_cur.pos < body.size()) {
      PendingMessage msg;
      if (!decode_message(&body_cur, &msg)) {
        break;
      }
      current_chunk.push_back(std::move(msg));
    }
  }
};

// NOLINTNEXTLINE(readability-function-size): monolithic by design (single read pass)
std::optional<RecordReader> RecordReader::open(const std::string& path) {
  auto* file = std::fopen(path.c_str(), "rb");  // NOLINT(concurrency-mt-unsafe)
  if (file == nullptr) {
    return std::nullopt;
  }
  std::vector<std::uint8_t> data;
  std::uint8_t chunk[65536];
  std::size_t n = 0;
  while ((n = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
    data.insert(data.end(), chunk,
                chunk + n);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }
  static_cast<void>(std::fclose(file));

  if (data.size() < kHeaderSize + kFooterSize) {
    return std::nullopt;
  }

  // Check header magic.
  Cursor hdr{.buf = &data, .pos = 0};
  const std::uint64_t magic = hdr.r_u64();
  if (magic != kMagicV2) {
    return std::nullopt;  // v0/v1 legacy: not supported by v2 reader yet
  }

  auto reader = RecordReader();
  reader.impl_ = std::make_shared<Impl>();
  reader.impl_->file_data = std::move(data);
  auto* impl = reader.impl_.get();

  // Re-read header fields.
  Cursor cur{.buf = &impl->file_data, .pos = 0};
  static_cast<void>(cur.r_u64());  // magic
  static_cast<void>(cur.r_u16());  // minor
  static_cast<void>(cur.r_u16());  // header_size
  static_cast<void>(cur.r_u16());  // flags
  static_cast<void>(cur.r_u16());  // reserved
  static_cast<void>(cur.r_u32());  // header_crc
  static_cast<void>(cur.r_u64());  // create_ts
  static_cast<void>(cur.r_u64());  // metadata_offset
  const std::uint64_t dict_offset = cur.r_u64();
  const std::uint64_t index_offset = cur.r_u64();
  const std::uint64_t stats_offset = cur.r_u64();
  static_cast<void>(cur.r_u32());  // chunk_count
  const std::uint32_t channel_count = cur.r_u32();
  static_cast<void>(cur.r_u64());  // total_messages
  static_cast<void>(cur.r_u64());  // total_bytes
  static_cast<void>(cur.r_u32());  // compression

  // Read footer.
  Cursor ftr{.buf = &impl->file_data, .pos = impl->file_data.size() - kFooterSize};
  static_cast<void>(ftr.r_u64());  // footer magic
  static_cast<void>(ftr.r_u64());  // index_offset (redundant)
  static_cast<void>(ftr.r_u64());  // stats_offset
  static_cast<void>(ftr.r_u64());  // dict_offset
  static_cast<void>(ftr.r_u32());  // file CRC
  static_cast<void>(ftr.r_u32());  // footer CRC
  static_cast<void>(ftr.r_u64());  // total_messages
  static_cast<void>(ftr.r_u64());  // total_bytes

  // --- Channel dictionary ---
  Cursor dict{.buf = &impl->file_data, .pos = static_cast<std::size_t>(dict_offset)};
  for (std::uint32_t i = 0; i < channel_count; ++i) {
    if (dict.r_u8() != static_cast<std::uint8_t>(RecordType::kChannelDict)) {
      break;
    }
    ChannelEntry ch;
    ch.id = dict.r_u16();
    const std::uint16_t name_len = dict.r_u16();
    const auto name_bytes = dict.r_bytes(name_len);
    ch.name.assign(name_bytes.begin(), name_bytes.end());
    ch.format = dict.r_u8();
    const std::uint16_t tn_len = dict.r_u16();
    const auto tn_bytes = dict.r_bytes(tn_len);
    ch.type_name.assign(tn_bytes.begin(), tn_bytes.end());
    const std::uint32_t schema_len = dict.r_u32();
    ch.schema_blob = dict.r_bytes(schema_len);
    static_cast<void>(dict.r_u32());  // CRC
    impl->channel_list.push_back(ch);
    impl->name_to_id[ch.name] = ch.id;
    impl->id_to_name[ch.id] = ch.name;
  }

  // --- Chunk index (if present) ---
  if (index_offset > 0 && index_offset < impl->file_data.size()) {
    Cursor idx{.buf = &impl->file_data, .pos = static_cast<std::size_t>(index_offset)};
    if (idx.r_u8() == static_cast<std::uint8_t>(RecordType::kChunkIndex)) {
      const std::uint32_t count = idx.r_u32();
      for (std::uint32_t i = 0; i < count; ++i) {
        ChunkIndexEntry entry;
        entry.chunk_offset = idx.r_u64();
        const auto bitmap_words = static_cast<std::size_t>(idx.r_u16());
        static_cast<void>(idx.r_bytes(static_cast<std::size_t>(bitmap_words) * 2));
        static_cast<void>(idx.r_u64());  // seq_min
        static_cast<void>(idx.r_u64());  // seq_max
        static_cast<void>(idx.r_u64());  // ts_first
        static_cast<void>(idx.r_u64());  // ts_last
        // Fill all channel IDs for now (bitmap not yet selective).
        for (const auto& ch : impl->channel_list) {
          entry.channel_ids.push_back(ch.id);
        }
        impl->chunks.push_back(entry);
      }
    }
  }

  // --- Statistics ---
  if (stats_offset > 0 && stats_offset < impl->file_data.size()) {
    Cursor st{.buf = &impl->file_data, .pos = static_cast<std::size_t>(stats_offset)};
    if (st.r_u8() == static_cast<std::uint8_t>(RecordType::kStatistics)) {
      const std::uint32_t ch_count = st.r_u32();
      for (std::uint32_t i = 0; i < ch_count; ++i) {
        const std::uint16_t id = st.r_u16();
        for (auto& ch : impl->channel_list) {
          if (ch.id == id) {
            ch.message_count = st.r_u64();
            ch.payload_bytes = st.r_u64();
            ch.seq_min = st.r_u64();
            ch.seq_max = st.r_u64();
            ch.ts_min = st.r_u64();
            ch.ts_max = st.r_u64();
            ch.avg_rate_hz = st.r_f64();
            break;
          }
        }
      }
      impl->stats.duration_ns = st.r_u64();
      impl->stats.total_messages = 0;
      for (const auto& ch : impl->channel_list) {
        impl->stats.total_messages += ch.message_count;
        impl->stats.total_bytes += ch.payload_bytes;
      }
      impl->stats.channels = impl->channel_list;
    }
  }

  // Data starts after the header (first chunk).
  impl->data_pos = kHeaderSize;

  return reader;
}

const std::vector<ChannelEntry>& RecordReader::channels() const { return impl_->channel_list; }

const ChannelEntry* RecordReader::find_channel(const std::string& name) const {
  const auto it = impl_->name_to_id.find(name);
  return it != impl_->name_to_id.end() ? find_channel(it->second) : nullptr;
}

const ChannelEntry* RecordReader::find_channel(std::uint16_t id) const {
  for (const auto& ch : impl_->channel_list) {
    if (ch.id == id) {
      return &ch;
    }
  }
  return nullptr;
}

bool RecordReader::next(RecordedMessageV2* msg) {
  auto* impl = impl_.get();
  if (impl == nullptr) {
    return false;
  }

  // Load next chunk if current is exhausted.
  while (impl->chunk_msg_pos >= impl->current_chunk.size()) {
    if (impl->data_pos >= impl->file_data.size() - kFooterSize) {
      return false;
    }
    // Data chunks are contiguous and precede the dictionary/index/stats.
    // Any other record type marks the end of the data section.
    if (impl->file_data.at(impl->data_pos) != static_cast<std::uint8_t>(RecordType::kDataChunk)) {
      return false;
    }
    impl->load_chunk_at(impl->data_pos);

    // Advance data_pos past this chunk.
    Cursor sz{.buf = &impl->file_data, .pos = impl->data_pos + 1};
    const std::uint32_t comp_len = sz.r_u32();
    const std::uint32_t uncomp_len = sz.r_u32();
    const std::uint8_t method = sz.r_u8();
    static_cast<void>(method);
    static_cast<void>(uncomp_len);
    // Record layout: type(1) + body_len(4) + uncomp_len(4) + method(1) + crc(4) +
    // ts_first(8) + ts_last(8) + compressed_data
    impl->data_pos += 1 + 4 + 4 + 1 + 4 + 8 + 8 + comp_len;

    if (impl->current_chunk.empty()) {
      continue;
    }
    impl->chunk_msg_pos = 0;
  }

  // Emit next message from current chunk.
  const auto& pending = impl->current_chunk[impl->chunk_msg_pos++];
  msg->channel_id = pending.channel_id;
  msg->seq = pending.seq;
  msg->ts_ns = pending.ts_ns;
  msg->payload = pending.payload;
  if (pending.lineage_blob.has_value()) {
    msg->lineage = deserialize_lineage(
        *pending.lineage_blob,
        [impl](std::uint16_t id) -> const std::string& { return impl->name_for(id); });
  } else {
    msg->lineage.reset();
  }
  return true;
}

void RecordReader::seek(std::uint64_t ts_ns) {
  auto* impl = impl_.get();
  if (impl == nullptr) {
    return;
  }
  // Reset and scan until we reach ts_ns.
  impl->data_pos = kHeaderSize;
  impl->current_chunk.clear();
  impl->chunk_msg_pos = 0;
  RecordedMessageV2 msg;
  while (next(&msg)) {
    if (msg.ts_ns >= ts_ns) {
      // We've consumed this message; push it back by decrementing.
      impl->chunk_msg_pos--;
      return;
    }
  }
}

std::vector<RecordedMessageV2> RecordReader::read_range(std::uint16_t channel_id,
                                                        std::uint64_t ts_lo, std::uint64_t ts_hi) {
  std::vector<RecordedMessageV2> out;
  RecordedMessageV2 msg;
  while (next(&msg)) {
    if (msg.ts_ns < ts_lo) {
      continue;
    }
    if (msg.ts_ns > ts_hi) {
      break;
    }
    if (msg.channel_id == channel_id) {
      out.push_back(msg);
    }
  }
  return out;
}

const RecordStats& RecordReader::stats() const { return impl_->stats; }

std::uint16_t RecordReader::major_version() const { return impl_->major_ver; }

const std::vector<ChunkIndexEntry>& RecordReader::chunk_index() const { return impl_->chunks; }

bool RecordReader::merge(const std::vector<std::string>& inputs, const std::string& output) {
  if (inputs.empty()) {
    return false;
  }
  // Open all readers.
  std::vector<RecordReader> readers;
  for (const auto& path : inputs) {
    auto reader = open(path);
    if (!reader.has_value()) {
      return false;
    }
    readers.push_back(std::move(*reader));
  }

  RecordWriter writer(output);
  // Merge channel dictionaries.
  std::unordered_map<std::string, std::uint16_t> global_ids;
  for (const auto& reader : readers) {
    for (const auto& ch : reader.channels()) {
      if (!global_ids.contains(ch.name)) {
        const auto new_id = writer.add_channel(ch.name, ch.format, ch.type_name, ch.schema_blob);
        global_ids[ch.name] = new_id;
      }
    }
  }

  // Collect all messages from all readers.
  struct TaggedMessage {
    RecordedMessageV2 msg;
    std::size_t reader_index;
  };
  std::vector<TaggedMessage> all;
  for (std::size_t i = 0; i < readers.size(); ++i) {
    RecordedMessageV2 msg;
    while (readers[i].next(&msg)) {
      all.push_back({.msg = msg, .reader_index = i});
    }
  }

  // Sort by timestamp.
  std::ranges::sort(all, [](const TaggedMessage& a, const TaggedMessage& b) {
    return a.msg.ts_ns < b.msg.ts_ns;
  });

  // Write merged.
  for (const auto& tagged : all) {
    const auto& ch = readers[tagged.reader_index].find_channel(tagged.msg.channel_id);
    if (ch == nullptr) {
      continue;
    }
    const auto global_id = global_ids.at(ch->name);
    writer.append(global_id, tagged.msg.seq, tagged.msg.ts_ns, tagged.msg.payload.data(),
                  tagged.msg.payload.size(),
                  tagged.msg.lineage.has_value() ? &*tagged.msg.lineage : nullptr);
  }
  return writer.finish();
}

bool RecordReader::split_by_time(const std::string& input, const std::string& output,
                                 std::uint64_t ts_lo, std::uint64_t ts_hi) {
  auto reader_opt = open(input);
  if (!reader_opt.has_value()) {
    return false;
  }
  auto& reader = *reader_opt;

  RecordWriter writer(output);
  std::unordered_map<std::uint16_t, std::uint16_t> id_map;
  for (const auto& ch : reader.channels()) {
    id_map[ch.id] = writer.add_channel(ch.name, ch.format, ch.type_name, ch.schema_blob);
  }

  RecordedMessageV2 msg;
  while (reader.next(&msg)) {
    if (msg.ts_ns < ts_lo || msg.ts_ns > ts_hi) {
      continue;
    }
    const auto it = id_map.find(msg.channel_id);
    if (it == id_map.end()) {
      continue;
    }
    writer.append(it->second, msg.seq, msg.ts_ns, msg.payload.data(), msg.payload.size(),
                  msg.lineage.has_value() ? &*msg.lineage : nullptr);
  }
  return writer.finish();
}

}  // namespace tianshu::dsl::record
