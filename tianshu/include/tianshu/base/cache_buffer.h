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

// Thread-safe circular buffer with overwrite-oldest-when-full policy.
//
// Design (per L4-PRIM-2, ADR-0010):
//   - Fixed capacity, pre-allocated vector storage
//   - fill() overwrites oldest element when buffer is full
//   - try_fetch() returns pointer (valid until next fill/try_fetch)
//   - SHM-compatible via index-based head/tail (per ADR-0010 appendix)
//
// Usage:
//   CacheBuffer<Message> buf(16);
//   buf.fill(msg);
//   Message* p = buf.try_fetch();
//   if (p) { process(*p); }

#pragma once

#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

namespace tianshu::base {

// Type-erased base so DataDispatcher can fill buffers without knowing T
// (per L4-CORE-6). Byte-oriented fill requires trivially copyable T.
class CacheBufferBase {
 public:
  virtual ~CacheBufferBase() = default;
  virtual void fill_bytes(const void* data, std::size_t size) = 0;
};

template <typename T>
class CacheBuffer : public CacheBufferBase {
 public:
  explicit CacheBuffer(std::size_t capacity) : capacity_(capacity), buffer_(capacity) {}

  void fill_bytes(const void* data, std::size_t /*size*/) override {
    fill(*static_cast<const T*>(data));
  }

  // Thread-safe. Overwrites oldest if full.
  void fill(T value) {
    std::scoped_lock lock(mutex_);
    buffer_[head_] = std::move(value);
    head_ = (head_ + 1) % capacity_;
    if (count_ < capacity_) {
      ++count_;
    } else {
      tail_ = (tail_ + 1) % capacity_;
    }
  }

  // Thread-safe. Returns nullptr if empty.
  // Pointer is valid until next fill/try_fetch on this buffer.
  T* try_fetch() {
    std::scoped_lock lock(mutex_);
    if (count_ == 0) {
      return nullptr;
    }
    T* result = &buffer_[tail_];
    tail_ = (tail_ + 1) % capacity_;
    --count_;
    return result;
  }

  // Thread-safe. Returns nullptr if empty. Does not consume.
  const T* observe() const {
    std::scoped_lock lock(mutex_);
    if (count_ == 0) {
      return nullptr;
    }
    return &buffer_[tail_];
  }

  bool empty() const {
    std::scoped_lock lock(mutex_);
    return count_ == 0;
  }

  bool full() const {
    std::scoped_lock lock(mutex_);
    return count_ == capacity_;
  }

  std::size_t size() const {
    std::scoped_lock lock(mutex_);
    return count_;
  }

  std::size_t capacity() const { return capacity_; }

 private:
  std::size_t capacity_;
  std::vector<T> buffer_;
  std::size_t head_{0};
  std::size_t tail_{0};
  std::size_t count_{0};
  mutable std::mutex mutex_;
};

}  // namespace tianshu::base
