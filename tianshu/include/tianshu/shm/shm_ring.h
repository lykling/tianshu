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

// Lock-free SPSC byte ring laid over caller-provided memory.
//
// Design (per L4-TRANS-3, ADR-0010):
//   - Single producer pushes, single consumer pops (per-reader ring)
//   - Message layout: [8B size][8B seq][8B timestamp][payload padded to 8]
//   - size 0 marks a wrap skip; the reader jumps to the ring start
//   - When full, push drops the new message (slow-consumer semantics)
//   - Memory ordering: release on write_pos after payload, acquire on
//     write_pos before reading payload; symmetric for read_pos

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace tianshu::shm {

class SpscRing {
 public:
  struct Header {
    std::atomic<std::uint64_t> write_pos;
    std::atomic<std::uint64_t> read_pos;
    std::uint64_t capacity;
    std::uint64_t magic;
  };

  static constexpr std::uint64_t kMagic = 0x52504353'474e4952ULL;
  static constexpr std::size_t kPrefixSize = 24;

  struct Metadata {
    std::uint64_t seq;
    std::int64_t timestamp_ns;
  };

  static std::size_t total_size(std::size_t capacity) {
    return sizeof(Header) + align_up(capacity, 8);
  }

  static SpscRing create(void* mem, std::size_t capacity) {
    auto* hdr = static_cast<Header*>(mem);
    hdr->write_pos.store(0, std::memory_order_relaxed);
    hdr->read_pos.store(0, std::memory_order_relaxed);
    hdr->capacity = align_up(capacity, 8);
    hdr->magic = kMagic;
    return SpscRing(mem);
  }

  static SpscRing attach(void* mem) { return SpscRing(mem); }

  // Returns false (drops) when the ring cannot fit the message.
  bool push(const void* data, std::size_t size, const Metadata& meta) {
    const std::size_t payload = align_up(size, 8);
    const std::size_t needed = kPrefixSize + payload;
    const std::uint64_t capacity = hdr_->capacity;

    std::uint64_t w = hdr_->write_pos.load(std::memory_order_relaxed);
    const std::uint64_t r = hdr_->read_pos.load(std::memory_order_acquire);

    if (w - r + needed > capacity) {
      return false;
    }

    std::uint64_t pos = w % capacity;
    if (pos + needed > capacity) {
      store_prefix(pos, 0, 0, 0);
      w += capacity - pos;
      pos = 0;
      if (w - r + needed > capacity) {
        return false;
      }
    }

    store_prefix(pos, size, meta.seq, static_cast<std::uint64_t>(meta.timestamp_ns));
    if (data != nullptr && size > 0) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      std::memcpy(bytes() + pos + kPrefixSize, data, size);
    }
    hdr_->write_pos.store(w + needed, std::memory_order_release);
    return true;
  }

  // Pops the oldest message into `out`. Returns false when empty.
  bool pop(std::vector<std::uint8_t>* out, Metadata* meta) {
    const std::uint64_t capacity = hdr_->capacity;
    while (true) {
      const std::uint64_t w = hdr_->write_pos.load(std::memory_order_acquire);
      const std::uint64_t r = hdr_->read_pos.load(std::memory_order_relaxed);
      if (r == w) {
        return false;
      }

      const std::uint64_t pos = r % capacity;
      std::uint64_t size = 0;
      std::uint64_t seq = 0;
      std::uint64_t ts = 0;
      load_prefix(pos, &size, &seq, &ts);
      if (size == 0) {
        hdr_->read_pos.store(r + (capacity - pos), std::memory_order_release);
        continue;
      }

      out->resize(static_cast<std::size_t>(size));
      if (size > 0) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        std::memcpy(out->data(), bytes() + pos + kPrefixSize, static_cast<std::size_t>(size));
      }
      meta->seq = seq;
      meta->timestamp_ns = static_cast<std::int64_t>(ts);
      hdr_->read_pos.store(r + kPrefixSize + align_up(static_cast<std::size_t>(size), 8),
                           std::memory_order_release);
      return true;
    }
  }

  [[nodiscard]] bool empty() const {
    return hdr_->write_pos.load(std::memory_order_acquire) ==
           hdr_->read_pos.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::uint64_t used() const {
    return hdr_->write_pos.load(std::memory_order_acquire) -
           hdr_->read_pos.load(std::memory_order_acquire);
  }

 private:
  explicit SpscRing(void* mem) : hdr_(static_cast<Header*>(mem)) {}

  static std::size_t align_up(std::size_t v, std::size_t a) { return (v + a - 1) & ~(a - 1); }

  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
  std::uint8_t* bytes() { return reinterpret_cast<std::uint8_t*>(hdr_) + sizeof(Header); }
  // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  void store_prefix(std::uint64_t pos, std::uint64_t size, std::uint64_t seq, std::uint64_t ts) {
    std::uint8_t* p = bytes() + pos;
    std::memcpy(p, &size, 8);
    std::memcpy(p + 8, &seq, 8);
    std::memcpy(p + 16, &ts, 8);
  }

  void load_prefix(std::uint64_t pos, std::uint64_t* size, std::uint64_t* seq,
                   std::uint64_t* ts) const {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* p = reinterpret_cast<const std::uint8_t*>(hdr_) + sizeof(Header) + pos;
    std::memcpy(size, p, 8);
    std::memcpy(seq, p + 8, 8);
    std::memcpy(ts, p + 16, 8);
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

  Header* hdr_;
};

}  // namespace tianshu::shm
