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

// Named shared-memory segment: create-or-attach with RAII lifetime.
//
// Design (per L4-TRANS-3, ADR-0010):
//   - shm_open(O_CREAT|O_EXCL) winner initializes the header and publishes it
//   - losers poll the magic word, then mmap the full size
//   - refcount in the header tracks live mappings; last one out unlinks
//   - data() returns the payload region; the SegmentHeader is private to
//     this class so users cannot clobber magic/refcount

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace tianshu::shm {

struct SegmentHeader {
  std::atomic<std::uint64_t> magic;
  std::atomic<std::int32_t> refcount;
  std::uint64_t size;
};

class ShmSegment {
 public:
  static constexpr std::uint64_t kMagic = 0x54485355'3153484dULL;

  // `size` is the payload capacity; sizeof(SegmentHeader) is added on top.
  static std::unique_ptr<ShmSegment> open_or_create(std::string_view name, std::size_t size);

  // Attaches to an existing segment without creating; nullptr when absent.
  static std::unique_ptr<ShmSegment> open_existing(std::string_view name);

  ~ShmSegment();

  ShmSegment(const ShmSegment&) = delete;
  ShmSegment& operator=(const ShmSegment&) = delete;

  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  void* data() { return static_cast<char*>(base_) + sizeof(SegmentHeader); }
  [[nodiscard]] const void* data() const {
    return static_cast<const char*>(base_) + sizeof(SegmentHeader);
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

  [[nodiscard]] std::size_t size() const { return mapped_size_ - sizeof(SegmentHeader); }
  [[nodiscard]] std::string_view name() const { return {name_}; }

 private:
  ShmSegment() = default;
  bool init(std::string_view name, std::size_t size);
  bool create_new(std::size_t total);
  bool attach_existing();
  // Maps from an already-open descriptor: the fd keeps pointing at the
  // original object even if the name is unlinked and recreated by a
  // concurrent instance, which is exactly the window a name-based
  // re-open loses.
  bool map_fd(int fd, std::size_t payload_size);
  SegmentHeader* header() { return static_cast<SegmentHeader*>(base_); }

  void* base_{nullptr};
  std::size_t mapped_size_{0};
  char name_[64]{};
};

}  // namespace tianshu::shm
