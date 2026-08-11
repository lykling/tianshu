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

// SpinLock: atomic flag spin-wait lock for very short critical sections.
// TicketLock: fair FIFO spin lock with ticket dispensing.
//
// Design (per L4-PRIM-5):
//   - Both satisfy Lockable concept (std::lock_guard compatible)
//   - SpinLock: lowest latency, no fairness guarantee
//   - TicketLock: fair FIFO ordering, slightly higher latency

#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

namespace tianshu::base {

class SpinLock {
 public:
  SpinLock() = default;

  void lock() {
    while (flag_.test_and_set(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }

  bool try_lock() { return !flag_.test_and_set(std::memory_order_acquire); }

  void unlock() { flag_.clear(std::memory_order_release); }

 private:
  std::atomic_flag flag_;
};

class TicketLock {
 public:
  TicketLock() = default;

  void lock() {
    const auto my_ticket = ticket_.fetch_add(1, std::memory_order_relaxed);
    while (serving_.load(std::memory_order_acquire) != my_ticket) {
      std::this_thread::yield();
    }
  }

  bool try_lock() {
    auto t = ticket_.load(std::memory_order_relaxed);
    auto s = serving_.load(std::memory_order_relaxed);
    if (t != s) {
      return false;
    }
    return ticket_.compare_exchange_strong(t, t + 1, std::memory_order_acquire,
                                           std::memory_order_relaxed);
  }

  void unlock() { serving_.fetch_add(1, std::memory_order_release); }

 private:
  std::atomic<uint32_t> ticket_{0};
  std::atomic<uint32_t> serving_{0};
};

}  // namespace tianshu::base
