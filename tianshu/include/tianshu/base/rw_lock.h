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

// Reader-writer lock wrapper around std::shared_mutex with RAII guards.
//
// Design (per L4-PRIM-4):
//   - Multiple concurrent readers, exclusive writer
//   - RAII guards: ReadGuard / WriteGuard
//   - Phase 1 uses std::shared_mutex; Phase 2 may optimize with custom spin

#pragma once

#include <shared_mutex>

namespace tianshu::base {

class RWLock {
 public:
  RWLock() = default;

  RWLock(const RWLock&) = delete;
  RWLock& operator=(const RWLock&) = delete;

  void read_lock() { mutex_.lock_shared(); }
  void read_unlock() { mutex_.unlock_shared(); }
  void write_lock() { mutex_.lock(); }
  void write_unlock() { mutex_.unlock(); }

  bool try_read_lock() { return mutex_.try_lock_shared(); }
  bool try_write_lock() { return mutex_.try_lock(); }

 private:
  std::shared_mutex mutex_;
};

class ReadGuard {
 public:
  explicit ReadGuard(RWLock& lock) : lock_(lock) { lock_.read_lock(); }
  ~ReadGuard() { lock_.read_unlock(); }

  ReadGuard(const ReadGuard&) = delete;
  ReadGuard& operator=(const ReadGuard&) = delete;

 private:
  RWLock& lock_;
};

class WriteGuard {
 public:
  explicit WriteGuard(RWLock& lock) : lock_(lock) { lock_.write_lock(); }
  ~WriteGuard() { lock_.write_unlock(); }

  WriteGuard(const WriteGuard&) = delete;
  WriteGuard& operator=(const WriteGuard&) = delete;

 private:
  RWLock& lock_;
};

}  // namespace tianshu::base
