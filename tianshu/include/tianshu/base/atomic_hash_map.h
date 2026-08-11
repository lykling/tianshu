// Copyright 2026 The TIANSHU Team. All Rights Reserved.
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

// Thread-safe hash map using RWLock + std::unordered_map.
//
// Design (per L4-PRIM-3, ADR-0010):
//   - Multiple concurrent readers, exclusive writers
//   - Phase 1: RWLock-based (simple, correct, testable)
//   - Phase 2: may swap to lock-free open-addressing behind same interface
//   - find() returns a copy (not a reference) to avoid lock lifetime issues

#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <unordered_map>
#include <utility>

#include "tianshu/base/rw_lock.h"

namespace tianshu::base {

template <typename K, typename V, typename Hash = std::hash<K>,
          typename KeyEqual = std::equal_to<K>>
class AtomicHashMap {
 public:
  AtomicHashMap() = default;

  explicit AtomicHashMap(std::size_t reserve) {
    const WriteGuard guard(lock_);
    map_.reserve(reserve);
  }

  AtomicHashMap(const AtomicHashMap&) = delete;
  AtomicHashMap& operator=(const AtomicHashMap&) = delete;

  void insert(const K& key, V value) {
    const WriteGuard guard(lock_);
    map_[key] = std::move(value);
  }

  void insert(K&& key, V value) {
    const WriteGuard guard(lock_);
    map_[std::move(key)] = std::move(value);
  }

  // Returns copy of value if found, nullopt otherwise.
  std::optional<V> find(const K& key) const {
    const ReadGuard guard(lock_);
    auto it = map_.find(key);
    if (it == map_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  bool contains(const K& key) const {
    const ReadGuard guard(lock_);
    return map_.count(key) > 0;
  }

  bool erase(const K& key) {
    const WriteGuard guard(lock_);
    return map_.erase(key) > 0;
  }

  std::size_t size() const {
    const ReadGuard guard(lock_);
    return map_.size();
  }

  bool empty() const {
    const ReadGuard guard(lock_);
    return map_.empty();
  }

  void clear() {
    const WriteGuard guard(lock_);
    map_.clear();
  }

  // Apply a read-only function to each entry under read lock.
  template <typename Fn>
  void for_each(Fn&& fn) const {
    const ReadGuard guard(lock_);
    for (const auto& [key, value] : map_) {
      fn(key, value);
    }
  }

 private:
  std::unordered_map<K, V, Hash, KeyEqual> map_;
  mutable RWLock lock_;
};

}  // namespace tianshu::base
