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

// Unit tests for ObjectPool<T> (L4-PRIM-1).

#include "tianshu/base/object_pool.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

struct TestData {
  int32_t value{0};
  double extra{0.0};
};

TEST(ObjectPoolTest, AcquireFromEmptyPoolReturnsNull) {
  tianshu::base::ObjectPool<TestData> pool(0);
  EXPECT_EQ(pool.acquire(), nullptr);
}

TEST(ObjectPoolTest, AcquireReleaseBasic) {
  tianshu::base::ObjectPool<TestData> pool(4);
  TestData* a = pool.acquire();
  ASSERT_NE(a, nullptr);
  a->value = 42;
  pool.release(a);

  TestData* b = pool.acquire();
  ASSERT_NE(b, nullptr);
  pool.release(b);
}

TEST(ObjectPoolTest, ExhaustPoolReturnsNull) {
  tianshu::base::ObjectPool<TestData> pool(2);
  TestData* a = pool.acquire();
  TestData* b = pool.acquire();
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(pool.acquire(), nullptr);
  pool.release(a);
  pool.release(b);
}

TEST(ObjectPoolTest, ReleaseNullIsNoop) {
  tianshu::base::ObjectPool<TestData> pool(2);
  pool.release(nullptr);
  EXPECT_EQ(pool.available(), 2);
}

TEST(ObjectPoolTest, AvailableCount) {
  tianshu::base::ObjectPool<TestData> pool(4);
  EXPECT_EQ(pool.available(), 4);
  TestData* a = pool.acquire();
  EXPECT_EQ(pool.available(), 3);
  pool.release(a);
  EXPECT_EQ(pool.available(), 4);
}

TEST(ObjectPoolTest, Capacity) {
  const tianshu::base::ObjectPool<TestData> pool(8);
  EXPECT_EQ(pool.capacity(), 8U);
}

TEST(ObjectPoolTest, DataPersistsAcrossAcquire) {
  tianshu::base::ObjectPool<TestData> pool(2);
  TestData* a = pool.acquire();
  a->value = 99;
  pool.release(a);

  TestData* b = pool.acquire();
  EXPECT_EQ(b->value, 99);
  pool.release(b);
}

TEST(ObjectPoolTest, PooledPtrAutoRelease) {
  tianshu::base::ObjectPool<TestData> pool(2);
  EXPECT_EQ(pool.available(), 2);
  {
    auto ptr = tianshu::base::make_pooled(pool);
    ASSERT_TRUE(ptr);
    ptr->value = 77;
    EXPECT_EQ(pool.available(), 1);
  }
  EXPECT_EQ(pool.available(), 2);
}

TEST(ObjectPoolTest, PooledPtrMoveSemantics) {
  tianshu::base::ObjectPool<TestData> pool(2);
  auto ptr1 = tianshu::base::make_pooled(pool);
  ASSERT_TRUE(ptr1);
  ptr1->value = 55;

  auto ptr2 = std::move(ptr1);
  ASSERT_TRUE(ptr2);
  EXPECT_EQ(ptr2->value, 55);
  // Move transferred ownership; verify pool state unchanged.
  EXPECT_EQ(pool.available(), 1);
}

TEST(ObjectPoolTest, ConcurrentAcquireRelease) {
  constexpr int kThreads = 4;
  constexpr int kIterations = 10000;
  tianshu::base::ObjectPool<TestData> pool(static_cast<std::size_t>(kThreads) * 2);

  std::atomic<int> success_count{0};
  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(kThreads));

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&]() {
      for (int i = 0; i < kIterations; ++i) {
        TestData* p = pool.acquire();
        if (p != nullptr) {
          p->value = i;
          pool.release(p);
          success_count.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  for (auto& th : threads) {
    th.join();
  }

  EXPECT_EQ(success_count.load(), kThreads * kIterations);
  EXPECT_EQ(pool.available(), pool.capacity());
}

}  // namespace
