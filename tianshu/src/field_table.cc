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

#include "tianshu/core/field_table.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

namespace tianshu::core {
namespace {

// POD layout assumption: offsets/widths come from the compiled type on the
// same ABI; v0 documents little-endian x86-64 only (ADR-0020 Phase 1).
std::size_t type_width(FieldType t) {
  switch (t) {
    case FieldType::kDouble:
    case FieldType::kInt64:
    case FieldType::kUint64:
      return 8;
    case FieldType::kFloat:
    case FieldType::kInt32:
    case FieldType::kUint32:
      return 4;
    case FieldType::kBool:
      return 1;
  }
  return 0;
}

double read_double(const std::uint8_t* p) {
  double v = 0.0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

float read_float(const std::uint8_t* p) {
  float v = 0.0F;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

std::int32_t read_i32(const std::uint8_t* p) {
  std::int32_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

std::int64_t read_i64(const std::uint8_t* p) {
  std::int64_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

std::uint32_t read_u32(const std::uint8_t* p) {
  std::uint32_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

std::uint64_t read_u64(const std::uint8_t* p) {
  std::uint64_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

void format_one(const FieldDesc& f, const std::uint8_t* p, char* buf, std::size_t bufsz) {
  switch (f.type) {
    case FieldType::kDouble:
      static_cast<void>(std::snprintf(buf, bufsz, "%.6g", read_double(p)));
      break;
    case FieldType::kFloat:
      static_cast<void>(std::snprintf(buf, bufsz, "%.6g", static_cast<double>(read_float(p))));
      break;
    case FieldType::kInt32:
      static_cast<void>(std::snprintf(buf, bufsz, "%d", read_i32(p)));
      break;
    case FieldType::kInt64:
      static_cast<void>(std::snprintf(  // NOLINT
          buf, bufsz, "%lld", static_cast<long long>(read_i64(p))));
      break;
    case FieldType::kUint32:
      static_cast<void>(std::snprintf(buf, bufsz, "%u", read_u32(p)));
      break;
    case FieldType::kUint64:
      static_cast<void>(std::snprintf(  // NOLINT
          buf, bufsz, "%llu", static_cast<unsigned long long>(read_u64(p))));
      break;
    case FieldType::kBool:
      static_cast<void>(std::snprintf(buf, bufsz, "%s", *p != 0 ? "true" : "false"));
      break;
  }
}

}  // namespace

FieldTreeView decode_pod(std::string_view type_name, const FieldDesc* fields,
                         std::size_t field_count, const std::uint8_t* payload,
                         std::size_t payload_size) {
  FieldTreeView view;
  view.type_name = std::string(type_name);
  view.fields.reserve(field_count);
  char buf[48];
  for (std::size_t i = 0; i < field_count; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const FieldDesc& f = fields[i];
    const std::size_t width = type_width(f.type);
    // Defensive against schema drift: skip fields that fall off the payload.
    if (f.offset > payload_size || (width * f.count) > (payload_size - f.offset)) {
      continue;
    }
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const std::uint8_t* base = payload + f.offset;
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (f.count == 1) {
      format_one(f, base, buf, sizeof(buf));
      view.fields.push_back({.name = f.name, .text = buf});
    } else {
      // Inline array: "[v0, v1, ...]" (truncated to 16 elements).
      std::string text = "[";
      const std::size_t shown = std::min<std::size_t>(f.count, 16);
      for (std::size_t e = 0; e < shown; ++e) {
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        format_one(f, base + (e * width), buf, sizeof(buf));
        // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        if (e != 0) {
          text += ", ";
        }
        text += buf;
      }
      if (f.count > shown) {
        text += ", ...";
      }
      text += "]";
      view.fields.push_back({.name = f.name, .text = text});
    }
  }
  return view;
}

// --- schema blob codec -----------------------------------------------------

namespace {

void append_bytes(std::vector<std::uint8_t>* out, const void* src, std::size_t n) {
  const auto* b = static_cast<const std::uint8_t*>(src);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  out->insert(out->end(), b, b + n);
}

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
bool read_bytes(const std::uint8_t* blob, std::size_t size, std::size_t* pos, void* dst,
                std::size_t n) {
  if (n > size - *pos) {
    return false;
  }
  std::memcpy(dst, blob + *pos, n);
  *pos += n;
  return true;
}
// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

constexpr std::uint32_t kMaxSchemaFields = 4096;

}  // namespace

std::vector<std::uint8_t> encode_pod_schema(std::string_view type_name, const FieldDesc* fields,
                                            std::size_t field_count) {
  std::vector<std::uint8_t> blob;
  blob.reserve(14U + type_name.size() + (field_count * 32U));
  append_bytes(&blob, &kPodSchemaMagic, sizeof(kPodSchemaMagic));
  const auto tn_len = static_cast<std::uint16_t>(type_name.size());
  append_bytes(&blob, &tn_len, sizeof(tn_len));
  blob.insert(blob.end(), type_name.begin(), type_name.end());
  const auto fc = static_cast<std::uint32_t>(field_count);
  append_bytes(&blob, &fc, sizeof(fc));
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  for (std::size_t i = 0; i < field_count; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const FieldDesc& f = fields[i];
    const auto name_len = static_cast<std::uint16_t>(std::strlen(f.name));
    append_bytes(&blob, &name_len, sizeof(name_len));
    append_bytes(&blob, f.name, name_len);
    const std::uint64_t offset = f.offset;
    append_bytes(&blob, &offset, sizeof(offset));
    const auto tag = static_cast<std::uint8_t>(f.type);
    append_bytes(&blob, &tag, sizeof(tag));
    const std::uint64_t count = f.count;
    append_bytes(&blob, &count, sizeof(count));
  }
  return blob;
}

bool decode_pod_schema(const std::uint8_t* blob, std::size_t size, OwnedSchemaTable* out) {
  if (size < sizeof(kPodSchemaMagic) + sizeof(std::uint16_t) + sizeof(std::uint32_t)) {
    return false;
  }
  std::size_t pos = 0;
  std::uint64_t magic = 0;
  if (!read_bytes(blob, size, &pos, &magic, sizeof(magic)) || magic != kPodSchemaMagic) {
    return false;
  }
  std::uint16_t tn_len = 0;
  if (!read_bytes(blob, size, &pos, &tn_len, sizeof(tn_len)) || tn_len == 0) {
    return false;
  }
  OwnedSchemaTable table;
  table.type_name.resize(tn_len);
  if (!read_bytes(blob, size, &pos, table.type_name.data(), tn_len)) {
    return false;
  }
  std::uint32_t fc = 0;
  if (!read_bytes(blob, size, &pos, &fc, sizeof(fc)) || fc > kMaxSchemaFields) {
    return false;
  }
  table.fields.reserve(fc);
  for (std::uint32_t i = 0; i < fc; ++i) {
    std::uint16_t name_len = 0;
    if (!read_bytes(blob, size, &pos, &name_len, sizeof(name_len)) || name_len == 0) {
      return false;
    }
    std::string name(name_len, '\0');
    if (!read_bytes(blob, size, &pos, name.data(), name_len)) {
      return false;
    }
    std::uint64_t offset = 0;
    std::uint8_t tag = 0;
    std::uint64_t count = 0;
    if (!read_bytes(blob, size, &pos, &offset, sizeof(offset)) ||
        !read_bytes(blob, size, &pos, &tag, sizeof(tag)) ||
        tag > static_cast<std::uint8_t>(FieldType::kBool) ||
        !read_bytes(blob, size, &pos, &count, sizeof(count))) {
      return false;
    }
    // deque keeps element addresses stable; the FieldDesc::name pointer
    // into names.back() survives later growth and container moves.
    table.names.emplace_back(std::move(name));
    table.fields.push_back({.name = table.names.back().c_str(),
                            .offset = offset,
                            .type = static_cast<FieldType>(tag),
                            .count = count});
  }
  *out = std::move(table);
  return true;
}

DecoderRegistry& DecoderRegistry::instance() noexcept {
  static DecoderRegistry reg;
  return reg;
}

void DecoderRegistry::register_fields(std::string_view type_name, const FieldDesc* fields,
                                      std::size_t count) noexcept {
  try {
    // Idempotent: a later registration replaces an earlier one for the
    // same type name (handles the same header included in many TUs).
    for (Entry& e : entries_) {
      if (e.type_name == type_name) {
        e.fields = fields;
        e.count = count;
        return;
      }
    }
    entries_.push_back(
        {.type_name = std::string(type_name), .fields = fields, .count = count, .owned = {}});
  } catch (...) {  // NOLINT(bugprone-empty-catch): registration is best-effort;
                   // decode() falls back to hex dump
  }
}

void DecoderRegistry::register_schema(OwnedSchemaTable&& table) noexcept {
  try {
    for (Entry& e : entries_) {
      if (e.type_name == table.type_name) {
        e.fields = nullptr;
        e.count = 0;
        e.owned = std::move(table);
        return;
      }
    }
    Entry entry{};
    entry.type_name = table.type_name;
    entry.owned = std::move(table);
    entries_.push_back(std::move(entry));
  } catch (...) {  // NOLINT(bugprone-empty-catch): registration is best-effort;
                   // decode() falls back to hex dump
  }
}

bool DecoderRegistry::decode(std::string_view type_name, const std::uint8_t* payload,
                             std::size_t payload_size, FieldTreeView* out) const {
  const auto it =
      std::ranges::find_if(entries_, [&](const Entry& e) { return e.type_name == type_name; });
  if (it == entries_.end()) {
    return false;
  }
  const bool owned = !it->owned.fields.empty();
  *out = decode_pod(type_name, owned ? it->owned.fields.data() : it->fields,
                    owned ? it->owned.fields.size() : it->count, payload, payload_size);
  return true;
}

bool DecoderRegistry::has(std::string_view type_name) const {
  return std::ranges::any_of(entries_, [&](const Entry& e) { return e.type_name == type_name; });
}

}  // namespace tianshu::core
