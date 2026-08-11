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

// Unit tests for RWLock (L4-PRIM-4).

#include "tianshu/base/rw_lock.h"

#include <atomic>
#include <thread>

#include <gtest/gtest.h>

namespace {

TEST(RWLockTest, MultipleReaders) {
  tianshu::base::RWLock lock;
  {
    const tianshu::base::ReadGuard r1(lock);
    const tianshu::base::ReadGuard r2(lock);
    const tianshu::base::ReadGuard r3(lock);
  }
}

TEST(RWLockTest, WriterExclusive) {
  tianshu::base::RWLock lock;
  lock.write_lock();
  EXPECT_FALSE(lock.try_read_lock());
  EXPECT_FALSE(lock.try_write_lock());
  lock.write_unlock();
  EXPECT_TRUE(lock.try_read_lock());
  lock.read_unlock();
}

TEST(RWLockTest, TryWriteLockFailsWhenReading) {
  tianshu::base::RWLock lock;
  const tianshu::base::ReadGuard r(lock);
  EXPECT_FALSE(lock.try_write_lock());
}

TEST(RWLockTest, WriteGuardRAII) {
  tianshu::base::RWLock lock;
  {
    const tianshu::base::WriteGuard w(lock);
    EXPECT_FALSE(lock.try_read_lock());
  }
  EXPECT_TRUE(lock.try_read_lock());
  lock.read_unlock();
}

TEST(RWLockTest, ConcurrentWritersCounter) {
  constexpr int kThreads = 4;
  constexpr int kIterations = 10000;
  tianshu::base::RWLock lock;
  std::atomic<int> counter{0};

  std::thread threads[kThreads];
  for (auto& t : threads) {
    t = std::thread([&]() {
      for (int i = 0; i < kIterations; ++i) {
        const tianshu::base::WriteGuard w(lock);
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
