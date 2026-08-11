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

// Lock-free object pool using Treiber stack (ABA-safe via tagged index).
//
// Design (per L4-PRIM-1, ADR-0010):
//   - Fixed capacity, pre-allocated contiguous storage
//   - acquire() returns nullptr when exhausted (non-blocking)
//   - release() pushes back to free-list (Treiber CAS with tag)
//   - ABA-safe: 64-bit packed head (32-bit tag + 32-bit index)
//
// Usage:
//   ObjectPool<Message> pool(1024);
//   auto ptr = make_pooled(pool);
//   if (ptr) { ptr->set_data(...); }

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace tianshu::base {

template <typename T>
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
class ObjectPool {
 public:
  explicit ObjectPool(std::size_t capacity)
      : capacity_(static_cast<Index>(capacity)), slots_(nullptr) {
    if (capacity_ == 0) {
      return;
    }
    slots_ = static_cast<Slot*>(::operator new[](capacity_ * sizeof(Slot)));

    for (Index i = 0; i < capacity_; ++i) {
      new (&slots_[i].storage) T();
      slots_[i].next = (i + 1 < capacity_) ? (i + 1) : kNil;
    }
    head_.store(pack(0, 0), std::memory_order_relaxed);
  }

  ~ObjectPool() {
    if (slots_ == nullptr) {
      return;
    }
    for (Index i = 0; i < capacity_; ++i) {
      reinterpret_cast<T*>(&slots_[i].storage)->~T();
    }
    ::operator delete[](slots_);
  }

  ObjectPool(const ObjectPool&) = delete;
  ObjectPool& operator=(const ObjectPool&) = delete;
  ObjectPool(ObjectPool&&) = delete;
  ObjectPool& operator=(ObjectPool&&) = delete;

  T* acquire() {
    Packed old_head = head_.load(std::memory_order_acquire);
    while (true) {
      Index idx = unpack_index(old_head);
      if (idx == kNil) {
        return nullptr;
      }
      Index next = slots_[idx].next.load(std::memory_order_relaxed);
      Packed new_head = pack(unpack_tag(old_head) + 1, next);
      if (head_.compare_exchange_weak(old_head, new_head, std::memory_order_acquire,
                                      std::memory_order_relaxed)) {
        return reinterpret_cast<T*>(&slots_[idx].storage);
      }
    }
  }

  // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
  void release(T* obj) {
    if (obj == nullptr) {
      return;
    }
    auto* slot = reinterpret_cast<Slot*>(obj);
    auto idx = static_cast<Index>(slot - slots_);
    if (idx >= capacity_) {
      return;
    }

    Packed old_head = head_.load(std::memory_order_relaxed);
    do {
      slot->next.store(unpack_index(old_head), std::memory_order_relaxed);
    } while (!head_.compare_exchange_weak(old_head, pack(unpack_tag(old_head) + 1, idx),
                                          std::memory_order_release, std::memory_order_relaxed));
  }
  // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

  std::size_t available() const {
    std::size_t count = 0;
    Index idx = unpack_index(head_.load(std::memory_order_relaxed));
    while (idx != kNil && count <= capacity_) {
      ++count;
      idx = slots_[idx].next.load(std::memory_order_relaxed);
    }
    return count;
  }

  std::size_t capacity() const { return capacity_; }

 private:
  using Index = uint32_t;
  using Packed = uint64_t;
  static constexpr Index kNil = static_cast<Index>(-1);

  static Packed pack(uint32_t tag, Index idx) { return (static_cast<Packed>(tag) << 32) | idx; }
  static Index unpack_index(Packed p) { return static_cast<Index>(p); }
  static uint32_t unpack_tag(Packed p) { return static_cast<uint32_t>(p >> 32); }

  struct Slot {
    std::aligned_storage_t<sizeof(T), alignof(T)> storage;
    std::atomic<Index> next{kNil};
  };

  Index capacity_;
  Slot* slots_;
  std::atomic<Packed> head_{pack(0, kNil)};
};
// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

template <typename T>
class PooledPtr {
 public:
  PooledPtr() : pool_(nullptr), ptr_(nullptr) {}
  PooledPtr(ObjectPool<T>* pool, T* ptr) : pool_(pool), ptr_(ptr) {}

  ~PooledPtr() {
    if (pool_ != nullptr && ptr_ != nullptr) {
      pool_->release(ptr_);
    }
  }

  PooledPtr(PooledPtr&& other) noexcept : pool_(other.pool_), ptr_(other.ptr_) {
    other.pool_ = nullptr;
    other.ptr_ = nullptr;
  }

  PooledPtr& operator=(PooledPtr&& other) noexcept {
    if (this != &other) {
      if (pool_ != nullptr && ptr_ != nullptr) {
        pool_->release(ptr_);
      }
      pool_ = other.pool_;
      ptr_ = other.ptr_;
      other.pool_ = nullptr;
      other.ptr_ = nullptr;
    }
    return *this;
  }

  PooledPtr(const PooledPtr&) = delete;
  PooledPtr& operator=(const PooledPtr&) = delete;

  T& operator*() const { return *ptr_; }
  T* operator->() const { return ptr_; }
  explicit operator bool() const { return ptr_ != nullptr; }
  T* get() const { return ptr_; }

 private:
  ObjectPool<T>* pool_;
  T* ptr_;
};

template <typename T>
PooledPtr<T> make_pooled(ObjectPool<T>& pool) {
  return PooledPtr<T>(&pool, pool.acquire());
}

}  // namespace tianshu::base
