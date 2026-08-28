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

#include "tianshu/transport/schema_sidecar.h"

#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "tianshu/shm/shm_segment.h"

namespace tianshu::transport::shm {
namespace {

constexpr std::uint64_t kSidecarMagic = 0x54485343'484d4132ULL;

struct SidecarHeader {
  std::atomic<std::uint64_t> magic;
  std::uint32_t blob_len;
};

std::size_t one_page() {
  static const auto PAGE = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
  return PAGE;
}

std::string hashed_name_for(std::string_view channel, const char* prefix) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const char c : channel) {
    hash ^= static_cast<std::uint8_t>(c);
    hash *= 1099511628211ULL;
  }
  char buf[48];
  static_cast<void>(std::snprintf(buf, sizeof(buf), "%s%016llx", prefix,  // NOLINT
                                  static_cast<unsigned long long>(hash)));
  return {buf};
}

}  // namespace

std::string segment_name_for(std::string_view channel) {
  return hashed_name_for(channel, "/tianshu_ch_");
}

std::string schema_segment_name_for(std::string_view channel) {
  return hashed_name_for(channel, "/tianshu_schema_");
}

void write_channel_schema(std::string_view channel, const std::uint8_t* blob, std::size_t size) {
  static std::mutex mutex;
  static std::unordered_map<std::string, std::unique_ptr<::tianshu::shm::ShmSegment>> keepalive;
  const std::string name = schema_segment_name_for(channel);
  const std::scoped_lock lock(mutex);
  if (keepalive.contains(name)) {
    return;
  }
  auto segment = ::tianshu::shm::ShmSegment::open_or_create(name, one_page());
  if (segment == nullptr) {
    return;
  }
  if (segment->size() < sizeof(SidecarHeader) + size) {
    return;
  }
  // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
  auto* payload = static_cast<std::uint8_t*>(segment->data());
  auto* hdr = reinterpret_cast<SidecarHeader*>(payload);
  // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
  hdr->blob_len = static_cast<std::uint32_t>(size);
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  std::memcpy(payload + sizeof(SidecarHeader), blob, size);
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  hdr->magic.store(kSidecarMagic, std::memory_order_release);
  keepalive[name] = std::move(segment);
}

bool read_channel_schema(std::string_view channel, std::vector<std::uint8_t>* blob) {
  auto segment = ::tianshu::shm::ShmSegment::open_existing(schema_segment_name_for(channel));
  if (segment == nullptr) {
    return false;
  }
  // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto* payload = static_cast<const std::uint8_t*>(segment->data());
  const auto* hdr = reinterpret_cast<const SidecarHeader*>(payload);
  // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
  if (hdr->magic.load(std::memory_order_acquire) != kSidecarMagic) {
    return false;
  }
  const std::size_t len = hdr->blob_len;
  if (len == 0 || len > segment->size() - sizeof(SidecarHeader)) {
    return false;
  }
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  blob->assign(payload + sizeof(SidecarHeader), payload + sizeof(SidecarHeader) + len);
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  return true;
}

}  // namespace tianshu::transport::shm
