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

// Address-independent pointer for shared memory (per L4-TRANS-24, ADR-0010).
//
// Design (Boost.Interprocess style, self-relative offset):
//   - Stores (target - this) instead of an absolute address, where `this`
//     is the offset_ptr instance itself (typically inside the SHM segment)
//   - Works regardless of where the segment is mapped in each process:
//     both endpoints shift by the same base relocation
//   - Zero extra overhead: get() is a single add
//   - offset 0 means null (a member can never validly point to itself)

#pragma once

#include <cstdint>

namespace tianshu::shm {

template <typename T>
class offset_ptr {
 public:
  offset_ptr() = default;

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  explicit offset_ptr(T* ptr)
      : offset_(ptr != nullptr ? static_cast<std::int64_t>(reinterpret_cast<const char*>(ptr) -
                                                           reinterpret_cast<const char*>(this))
                               : 0) {}

  offset_ptr(const offset_ptr& other)
      : offset_(other.offset_ != 0 ? offset_from_raw(other.get()) : 0) {}

  offset_ptr& operator=(const offset_ptr& other) {
    if (this != &other) {
      offset_ = other.offset_ != 0 ? offset_from_raw(other.get()) : 0;
    }
    return *this;
  }

  offset_ptr(offset_ptr&& other) noexcept
      : offset_(other.offset_ != 0 ? offset_from_raw(other.get()) : 0) {
    other.offset_ = 0;
  }

  offset_ptr& operator=(offset_ptr&& other) noexcept {
    if (this != &other) {
      offset_ = other.offset_ != 0 ? offset_from_raw(other.get()) : 0;
      other.offset_ = 0;
    }
    return *this;
  }

  offset_ptr& operator=(T* ptr) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    offset_ = ptr != nullptr ? static_cast<std::int64_t>(reinterpret_cast<const char*>(ptr) -
                                                         reinterpret_cast<const char*>(this))
                             : 0;
    return *this;
  }

  // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
  T* get() const {
    return offset_ == 0 ? nullptr
                        : reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(this) +
                                               static_cast<std::uintptr_t>(offset_));
  }
  // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

  T& operator*() const { return *get(); }
  T* operator->() const { return get(); }

  explicit operator bool() const { return offset_ != 0; }

  bool operator==(const offset_ptr& other) const { return get() == other.get(); }
  bool operator!=(const offset_ptr& other) const { return !(*this == other); }
  bool operator==(const T* ptr) const { return get() == ptr; }
  bool operator!=(const T* ptr) const { return get() != ptr; }

  std::int64_t raw_offset() const { return offset_; }

 private:
  // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
  std::int64_t offset_from_raw(const T* ptr) const {
    return static_cast<std::int64_t>(reinterpret_cast<const char*>(ptr) -
                                     reinterpret_cast<const char*>(this));
  }
  // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

  std::int64_t offset_{0};
};

}  // namespace tianshu::shm
