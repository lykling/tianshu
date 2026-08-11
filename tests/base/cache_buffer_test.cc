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

// Unit tests for CacheBuffer<T> (L4-PRIM-2).

#include "tianshu/base/cache_buffer.h"

#include <atomic>
#include <cstdint>
#include <thread>

#include <gtest/gtest.h>

namespace {

TEST(CacheBufferTest, EmptyOnCreate) {
  const tianshu::base::CacheBuffer<int32_t> buf(4);
  EXPECT_TRUE(buf.empty());
  EXPECT_FALSE(buf.full());
  EXPECT_EQ(buf.size(), 0U);
  EXPECT_EQ(buf.capacity(), 4U);
}

TEST(CacheBufferTest, FillAndFetch) {
  tianshu::base::CacheBuffer<int32_t> buf(4);
  buf.fill(42);
  EXPECT_EQ(buf.size(), 1U);
  auto* p = buf.try_fetch();
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(*p, 42);
  EXPECT_TRUE(buf.empty());
}

TEST(CacheBufferTest, TryFetchEmptyReturnsNull) {
  tianshu::base::CacheBuffer<int32_t> buf(4);
  EXPECT_EQ(buf.try_fetch(), nullptr);
}

TEST(CacheBufferTest, ObserveDoesNotConsume) {
  tianshu::base::CacheBuffer<int32_t> buf(4);
  buf.fill(99);
  const auto* p = buf.observe();
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(*p, 99);
  EXPECT_EQ(buf.size(), 1U);
}

TEST(CacheBufferTest, FifoOrder) {
  tianshu::base::CacheBuffer<int32_t> buf(4);
  for (int32_t i = 0; i < 3; ++i) {
    buf.fill(i);
  }
  for (int32_t i = 0; i < 3; ++i) {
    auto* p = buf.try_fetch();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, i);
  }
  EXPECT_TRUE(buf.empty());
}

TEST(CacheBufferTest, OverwriteOldestWhenFull) {
  tianshu::base::CacheBuffer<int32_t> buf(2);
  buf.fill(10);
  buf.fill(20);
  EXPECT_TRUE(buf.full());
  buf.fill(30);  // overwrites 10
  EXPECT_EQ(buf.size(), 2U);
  auto* p1 = buf.try_fetch();
  ASSERT_NE(p1, nullptr);
  EXPECT_EQ(*p1, 20);
  auto* p2 = buf.try_fetch();
  ASSERT_NE(p2, nullptr);
  EXPECT_EQ(*p2, 30);
  EXPECT_TRUE(buf.empty());
}

TEST(CacheBufferTest, WrapAroundMultipleCycles) {
  tianshu::base::CacheBuffer<int32_t> buf(3);
  for (int32_t cycle = 0; cycle < 5; ++cycle) {
    for (int32_t i = 0; i < 3; ++i) {
      buf.fill((cycle * 10) + i);
    }
    for (int32_t i = 0; i < 3; ++i) {
      auto* p = buf.try_fetch();
      ASSERT_NE(p, nullptr);
      EXPECT_EQ(*p, (cycle * 10) + i);
    }
  }
  EXPECT_TRUE(buf.empty());
}

TEST(CacheBufferTest, ConcurrentProducerConsumer) {
  constexpr int kIterations = 50000;
  tianshu::base::CacheBuffer<int32_t> buf(256);
  std::atomic<bool> producer_done{false};
  std::atomic<int> consumed{0};

  std::thread producer([&]() {
    for (int i = 0; i < kIterations; ++i) {
      buf.fill(i);
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::thread consumer([&]() {
    while (true) {
      if (buf.try_fetch() != nullptr) {
        consumed.fetch_add(1, std::memory_order_relaxed);
      } else if (producer_done.load(std::memory_order_acquire)) {
        break;
      }
    }
  });

  producer.join();
  consumer.join();

  // With overwrite policy, consumed may be < kIterations.
  EXPECT_LE(consumed.load(), kIterations);
  EXPECT_GT(consumed.load(), 0);
  EXPECT_TRUE(buf.empty());
}

}  // namespace
