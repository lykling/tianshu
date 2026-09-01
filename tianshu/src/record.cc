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

#include "tianshu/dsl/record.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace tianshu::dsl {
namespace {

constexpr std::uint64_t kRecordMagic = 0x54524543'30303031ULL;  // 'TREC0001'

void append_u32(std::vector<std::uint8_t>* out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out->push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
  }
}

void append_u64(std::vector<std::uint8_t>* out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out->push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
  }
}

void append_bytes(std::vector<std::uint8_t>* out, const void* data, std::size_t n) {
  const auto* b = static_cast<const std::uint8_t*>(data);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  out->insert(out->end(), b, b + n);
}

[[nodiscard]] bool read_u32(const std::vector<std::uint8_t>& buf, std::size_t* pos,
                            std::uint32_t* out) {
  if (*pos + 4 > buf.size()) {
    return false;
  }
  *out = 0;
  for (int i = 0; i < 4; ++i) {
    *out |= static_cast<std::uint32_t>(buf[*pos + static_cast<std::size_t>(i)]) << (8 * i);
  }
  *pos += 4;
  return true;
}

[[nodiscard]] bool read_u64(const std::vector<std::uint8_t>& buf, std::size_t* pos,
                            std::uint64_t* out) {
  if (*pos + 8 > buf.size()) {
    return false;
  }
  *out = 0;
  for (int i = 0; i < 8; ++i) {
    *out |= static_cast<std::uint64_t>(buf[*pos + static_cast<std::size_t>(i)]) << (8 * i);
  }
  *pos += 8;
  return true;
}

[[nodiscard]] bool read_bytes(const std::vector<std::uint8_t>& buf, std::size_t* pos, std::size_t n,
                              std::vector<std::uint8_t>* out) {
  if (*pos + n > buf.size()) {
    return false;
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  out->assign(buf.begin() + static_cast<std::ptrdiff_t>(*pos),
              buf.begin() + static_cast<std::ptrdiff_t>(*pos + n));
  *pos += n;
  return true;
}

}  // namespace

RecordFile::RecordFile(std::string path) : path_(std::move(path)) {}

void RecordFile::append(const RecordedMessage& msg) { records_.push_back(msg); }

bool RecordFile::save() const {
  std::vector<std::uint8_t> buf;
  append_u64(&buf, kRecordMagic);
  append_u32(&buf, static_cast<std::uint32_t>(records_.size()));
  for (const auto& rec : records_) {
    append_u32(&buf, static_cast<std::uint32_t>(rec.channel.size()));
    append_bytes(&buf, rec.channel.data(), rec.channel.size());
    append_u64(&buf, rec.seq);
    append_u32(&buf, static_cast<std::uint32_t>(rec.bytes.size()));
    append_bytes(&buf, rec.bytes.data(), rec.bytes.size());
    append_u32(&buf, static_cast<std::uint32_t>(rec.lineage_text.size()));
    append_bytes(&buf, rec.lineage_text.data(), rec.lineage_text.size());
  }

  auto* file = std::fopen(path_.c_str(), "wb");  // NOLINT(concurrency-mt-unsafe)
  if (file == nullptr) {
    return false;
  }
  const std::size_t written = std::fwrite(buf.data(), 1, buf.size(), file);
  static_cast<void>(std::fclose(file));
  return written == buf.size();
}

std::vector<RecordedMessage> RecordFile::load(const std::string& path) {
  std::vector<RecordedMessage> out;
  auto* file = std::fopen(path.c_str(), "rb");  // NOLINT(concurrency-mt-unsafe)
  if (file == nullptr) {
    return out;
  }
  std::vector<std::uint8_t> buf;
  std::vector<std::uint8_t> chunk(4096);
  std::size_t n = 0;
  while ((n = std::fread(chunk.data(), 1, chunk.size(), file)) > 0) {
    buf.insert(buf.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n));
  }
  static_cast<void>(std::fclose(file));

  std::size_t pos = 0;
  std::uint64_t magic = 0;
  if (!read_u64(buf, &pos, &magic) || magic != kRecordMagic) {
    return out;
  }
  std::uint32_t count = 0;
  if (!read_u32(buf, &pos, &count)) {
    return out;
  }
  out.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    RecordedMessage rec;
    std::uint32_t ch_len = 0;
    std::vector<std::uint8_t> tmp;
    if (!read_u32(buf, &pos, &ch_len) || !read_bytes(buf, &pos, ch_len, &tmp)) {
      return {};
    }
    rec.channel.assign(tmp.begin(), tmp.end());
    if (!read_u64(buf, &pos, &rec.seq)) {
      return {};
    }
    std::uint32_t len = 0;
    if (!read_u32(buf, &pos, &len) || !read_bytes(buf, &pos, len, &rec.bytes)) {
      return {};
    }
    if (!read_u32(buf, &pos, &len) || !read_bytes(buf, &pos, len, &tmp)) {
      return {};
    }
    rec.lineage_text.assign(tmp.begin(), tmp.end());
    out.push_back(std::move(rec));
  }
  return out;
}

}  // namespace tianshu::dsl
