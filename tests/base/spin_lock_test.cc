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

// Unit tests for SpinLock and TicketLock (L4-PRIM-5).

#include "tianshu/base/spin_lock.h"

#include <atomic>
#include <mutex>
#include <thread>

#include <gtest/gtest.h>

namespace {

TEST(SpinLockTest, LockUnlockBasic) {
  tianshu::base::SpinLock lock;
  lock.lock();
  EXPECT_FALSE(lock.try_lock());
  lock.unlock();
  EXPECT_TRUE(lock.try_lock());
  lock.unlock();
}

TEST(SpinLockTest, ScopedLockCompatible) {
  tianshu::base::SpinLock lock;
  { const std::scoped_lock guard(lock); }
  EXPECT_TRUE(lock.try_lock());
  lock.unlock();
}

TEST(SpinLockTest, TryLockReturnsFalseWhenLocked) {
  tianshu::base::SpinLock lock;
  lock.lock();
  EXPECT_FALSE(lock.try_lock());
  lock.unlock();
}

TEST(SpinLockTest, ConcurrentMutualExclusion) {
  constexpr int kThreads = 4;
  constexpr int kIterations = 10000;
  tianshu::base::SpinLock lock;
  std::atomic<int> counter{0};

  std::thread threads[kThreads];
  for (auto& t : threads) {
    t = std::thread([&]() {
      for (int i = 0; i < kIterations; ++i) {
        const std::scoped_lock guard(lock);
        counter.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }
  EXPECT_EQ(counter.load(), kThreads * kIterations);
}

TEST(TicketLockTest, LockUnlockBasic) {
  tianshu::base::TicketLock lock;
  lock.lock();
  EXPECT_FALSE(lock.try_lock());
  lock.unlock();
  EXPECT_TRUE(lock.try_lock());
  lock.unlock();
}

TEST(TicketLockTest, ScopedLockCompatible) {
  tianshu::base::TicketLock lock;
  { const std::scoped_lock guard(lock); }
  EXPECT_TRUE(lock.try_lock());
  lock.unlock();
}

TEST(TicketLockTest, ConcurrentMutualExclusion) {
  constexpr int kThreads = 4;
  constexpr int kIterations = 10000;
  tianshu::base::TicketLock lock;
  std::atomic<int> counter{0};

  std::thread threads[kThreads];
  for (auto& t : threads) {
    t = std::thread([&]() {
      for (int i = 0; i < kIterations; ++i) {
        const std::scoped_lock guard(lock);
        counter.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }
  EXPECT_EQ(counter.load(), kThreads * kIterations);
}

}  // namespace
