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

struct Trackable {
  static inline std::atomic<int> alive_count{0};
  int32_t data{0};
  Trackable() { alive_count.fetch_add(1); }
  ~Trackable() { alive_count.fetch_sub(1); }
};

TEST(ObjectPoolTest, AcquireFromEmptyPoolReturnsNull) {
  tianshu::base::ObjectPool<TestData> pool(0);
  EXPECT_EQ(pool.acquire(), nullptr);
  EXPECT_EQ(pool.available(), 0U);
}

TEST(ObjectPoolTest, EmptyPoolCapacity) {
  const tianshu::base::ObjectPool<TestData> pool(0);
  EXPECT_EQ(pool.capacity(), 0U);
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
  EXPECT_EQ(pool.available(), 0U);
  pool.release(a);
  pool.release(b);
}

TEST(ObjectPoolTest, ReleaseNullIsNoop) {
  tianshu::base::ObjectPool<TestData> pool(2);
  pool.release(nullptr);
  EXPECT_EQ(pool.available(), 2U);
}

TEST(ObjectPoolTest, AvailableCount) {
  tianshu::base::ObjectPool<TestData> pool(4);
  EXPECT_EQ(pool.available(), 4U);
  TestData* a = pool.acquire();
  EXPECT_EQ(pool.available(), 3U);
  pool.release(a);
  EXPECT_EQ(pool.available(), 4U);
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

TEST(ObjectPoolTest, AcquireAllAndReleaseAll) {
  tianshu::base::ObjectPool<TestData> pool(4);
  std::vector<TestData*> ptrs;
  for (std::size_t i = 0; i < 4; ++i) {
    auto* p = pool.acquire();
    ASSERT_NE(p, nullptr);
    p->value = static_cast<int32_t>(i);
    ptrs.push_back(p);
  }
  EXPECT_EQ(pool.available(), 0U);
  for (auto* p : ptrs) {
    pool.release(p);
  }
  EXPECT_EQ(pool.available(), 4U);
}

TEST(ObjectPoolTest, DestructorDestroysObjects) {
  Trackable::alive_count = 0;
  {
    tianshu::base::ObjectPool<Trackable> pool(3);
    EXPECT_EQ(Trackable::alive_count.load(), 3);
    auto* p = pool.acquire();
    EXPECT_EQ(Trackable::alive_count.load(), 3);
    pool.release(p);
  }
  EXPECT_EQ(Trackable::alive_count.load(), 0);
}

TEST(ObjectPoolTest, EmptyPoolDestructor) {
  Trackable::alive_count = 0;
  {
    tianshu::base::ObjectPool<Trackable> const pool(0);
    EXPECT_EQ(Trackable::alive_count.load(), 0);
  }
  EXPECT_EQ(Trackable::alive_count.load(), 0);
}

TEST(ObjectPoolTest, PooledPtrAutoRelease) {
  tianshu::base::ObjectPool<TestData> pool(2);
  EXPECT_EQ(pool.available(), 2U);
  {
    auto ptr = tianshu::base::make_pooled(pool);
    ASSERT_TRUE(ptr);
    ptr->value = 77;
    EXPECT_EQ(pool.available(), 1U);
  }
  EXPECT_EQ(pool.available(), 2U);
}

TEST(ObjectPoolTest, PooledPtrMoveSemantics) {
  tianshu::base::ObjectPool<TestData> pool(2);
  auto ptr1 = tianshu::base::make_pooled(pool);
  ASSERT_TRUE(ptr1);
  ptr1->value = 55;

  auto ptr2 = std::move(ptr1);
  ASSERT_TRUE(ptr2);
  EXPECT_EQ(ptr2->value, 55);
  EXPECT_EQ(pool.available(), 1U);
}

TEST(ObjectPoolTest, PooledPtrDefaultConstructor) {
  tianshu::base::PooledPtr<TestData> const ptr;
  EXPECT_FALSE(ptr);
  EXPECT_EQ(ptr.get(), nullptr);
}

TEST(ObjectPoolTest, PooledPtrMoveAssignment) {
  tianshu::base::ObjectPool<TestData> pool(4);
  auto ptr1 = tianshu::base::make_pooled(pool);
  ptr1->value = 111;

  auto ptr2 = tianshu::base::make_pooled(pool);
  ptr2->value = 222;

  ptr2 = std::move(ptr1);
  ASSERT_TRUE(ptr2);
  EXPECT_EQ(ptr2->value, 111);
  EXPECT_EQ(pool.available(), 3U);
}

TEST(ObjectPoolTest, PooledPtrDereference) {
  tianshu::base::ObjectPool<TestData> pool(2);
  auto ptr = tianshu::base::make_pooled(pool);
  ASSERT_TRUE(ptr);
  ptr->value = 444;
  EXPECT_EQ((*ptr).value, 444);
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
