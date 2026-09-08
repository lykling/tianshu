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

// Fixed-inline-capacity vector: heap-allocates only beyond N elements.
// Exists for the per-message hot path (ADR-0030 D8 L2b) — Lineage
// carries 1-2 branches and few hops on common chains, so the common
// copy/construct path becomes allocation-free. Deliberately minimal:
// no insert/erase/reserve shims; grow-only push_back, like the usage.

#pragma once

#include <cstddef>
#include <utility>

namespace tianshu::base {

template <typename T, std::size_t N>
class SmallVec {
 public:
  using value_type = T;
  using iterator = T*;
  using const_iterator = const T*;

  SmallVec() = default;

  SmallVec(const SmallVec& other) { copy_from(other); }

  SmallVec(SmallVec&& other) noexcept { move_from(std::move(other)); }

  SmallVec& operator=(const SmallVec& other) {
    if (this != &other) {
      destroy_elements();
      size_ = 0;
      copy_from(other);
    }
    return *this;
  }

  SmallVec& operator=(SmallVec&& other) noexcept {
    if (this != &other) {
      destroy_elements();
      release_heap();
      size_ = 0;
      capacity_ = N;
      move_from(std::move(other));
    }
    return *this;
  }

  ~SmallVec() {
    destroy_elements();
    release_heap();
  }

  void push_back(const T& value) {
    if (size_ == capacity_) {
      grow();
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    new (slot(size_)) T(value);
    ++size_;
  }

  void push_back(T&& value) {
    if (size_ == capacity_) {
      grow();
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    new (slot(size_)) T(std::move(value));
    ++size_;
  }

  [[nodiscard]] T& operator[](std::size_t i) { return data()[i]; }
  [[nodiscard]] const T& operator[](std::size_t i) const { return data()[i]; }

  [[nodiscard]] T& front() { return data()[0]; }
  [[nodiscard]] const T& front() const { return data()[0]; }
  [[nodiscard]] T& back() { return data()[size_ - 1]; }
  [[nodiscard]] const T& back() const { return data()[size_ - 1]; }

  [[nodiscard]] bool empty() const { return size_ == 0; }
  [[nodiscard]] std::size_t size() const { return size_; }

  [[nodiscard]] iterator begin() { return data(); }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  [[nodiscard]] iterator end() { return data() + size_; }
  [[nodiscard]] const_iterator begin() const { return data(); }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  [[nodiscard]] const_iterator end() const { return data() + size_; }

 private:
  [[nodiscard]] T* data() {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return heap_ != nullptr ? heap_ : reinterpret_cast<T*>(inline_);
  }
  [[nodiscard]] const T* data() const {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return heap_ != nullptr ? heap_ : reinterpret_cast<const T*>(inline_);
  }

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-bounds-pointer-arithmetic)
  [[nodiscard]] void* slot(std::size_t i) { return data() + i; }

  void grow() {
    const std::size_t new_capacity = capacity_ * 2;
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    auto* fresh = new T[new_capacity];
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    for (std::size_t i = 0; i < size_; ++i) {
      fresh[i] = T(std::move(data()[i]));
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    destroy_elements();
    release_heap();
    heap_ = fresh;
    capacity_ = new_capacity;
  }

  void copy_from(const SmallVec& other) {
    for (std::size_t i = 0; i < other.size_; ++i) {
      push_back(other[i]);
    }
  }

  void move_from(SmallVec&& other) {
    if (other.heap_ == nullptr) {
      for (std::size_t i = 0; i < other.size_; ++i) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-bounds-pointer-arithmetic)
        new (slot(i)) T(std::move(reinterpret_cast<T*>(other.inline_)[i]));
      }
      size_ = other.size_;
      other.destroy_elements();
      other.size_ = 0;
    } else {
      heap_ = std::exchange(other.heap_, nullptr);
      capacity_ = std::exchange(other.capacity_, N);
      size_ = std::exchange(other.size_, 0);
    }
  }

  void destroy_elements() {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    for (std::size_t i = 0; i < size_; ++i) {
      data()[i].~T();
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }

  void release_heap() {
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    delete[] heap_;
    heap_ = nullptr;
    capacity_ = N;
  }

  std::size_t size_{0};
  std::size_t capacity_{N};
  T* heap_{nullptr};
  alignas(T) unsigned char inline_[N * sizeof(T)]{};
};

}  // namespace tianshu::base
