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

// Unit tests for AtomicHashMap<K,V> (L4-PRIM-3).

#include "tianshu/base/atomic_hash_map.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

#include <gtest/gtest.h>

namespace {

TEST(AtomicHashMapTest, EmptyOnCreate) {
  const tianshu::base::AtomicHashMap<int32_t, std::string> map;
  EXPECT_TRUE(map.empty());
  EXPECT_EQ(map.size(), 0U);
}

TEST(AtomicHashMapTest, ReserveConstructor) {
  tianshu::base::AtomicHashMap<int32_t, std::string> map(64);
  EXPECT_TRUE(map.empty());
  EXPECT_EQ(map.size(), 0U);
  map.insert(1, "a");
  EXPECT_EQ(map.size(), 1U);
}

TEST(AtomicHashMapTest, InsertAndFind) {
  tianshu::base::AtomicHashMap<int32_t, std::string> map;
  map.insert(42, "hello");
  EXPECT_EQ(map.size(), 1U);
  auto val = map.find(42);
  ASSERT_TRUE(val.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access): ASSERT_TRUE guarantees non-empty
  EXPECT_EQ(*val, "hello");
}

TEST(AtomicHashMapTest, FindMissingReturnsNullopt) {
  const tianshu::base::AtomicHashMap<int32_t, std::string> map;
  auto val = map.find(999);
  EXPECT_FALSE(val.has_value());
}

TEST(AtomicHashMapTest, Contains) {
  tianshu::base::AtomicHashMap<int32_t, int32_t> map;
  map.insert(1, 100);
  EXPECT_TRUE(map.contains(1));
  EXPECT_FALSE(map.contains(2));
}

TEST(AtomicHashMapTest, Erase) {
  tianshu::base::AtomicHashMap<int32_t, int32_t> map;
  map.insert(1, 100);
  EXPECT_TRUE(map.erase(1));
  EXPECT_FALSE(map.contains(1));
  EXPECT_FALSE(map.erase(1));
}

TEST(AtomicHashMapTest, EraseNonExistentReturnsFalse) {
  tianshu::base::AtomicHashMap<int32_t, int32_t> map;
  map.insert(1, 100);
  EXPECT_FALSE(map.erase(999));
  EXPECT_EQ(map.size(), 1U);
}

TEST(AtomicHashMapTest, EraseFromEmptyMap) {
  tianshu::base::AtomicHashMap<int32_t, int32_t> map;
  EXPECT_FALSE(map.erase(1));
}

TEST(AtomicHashMapTest, OverwriteExisting) {
  tianshu::base::AtomicHashMap<int32_t, std::string> map;
  map.insert(1, "old");
  map.insert(1, "new");
  EXPECT_EQ(map.size(), 1U);
  auto val = map.find(1);
  ASSERT_TRUE(val.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access): ASSERT_TRUE guarantees non-empty
  EXPECT_EQ(*val, "new");
}

TEST(AtomicHashMapTest, Clear) {
  tianshu::base::AtomicHashMap<int32_t, int32_t> map;
  map.insert(1, 10);
  map.insert(2, 20);
  map.clear();
  EXPECT_TRUE(map.empty());
  EXPECT_EQ(map.size(), 0U);
}

TEST(AtomicHashMapTest, ClearEmptyMap) {
  tianshu::base::AtomicHashMap<int32_t, int32_t> map;
  map.clear();
  EXPECT_TRUE(map.empty());
}

TEST(AtomicHashMapTest, ForEach) {
  tianshu::base::AtomicHashMap<int32_t, int32_t> map;
  for (int32_t i = 0; i < 5; ++i) {
    map.insert(i, i * 10);
  }
  int32_t sum = 0;
  map.for_each([&]([[maybe_unused]] const int32_t& key, const int32_t& val) { sum += val; });
  EXPECT_EQ(sum, 100);
}

TEST(AtomicHashMapTest, ForEachEmptyMap) {
  const tianshu::base::AtomicHashMap<int32_t, int32_t> map;
  int32_t count = 0;
  map.for_each(
      [&]([[maybe_unused]] const int32_t& key, [[maybe_unused]] const int32_t& val) { ++count; });
  EXPECT_EQ(count, 0);
}

TEST(AtomicHashMapTest, ForEachModifiesExternalState) {
  tianshu::base::AtomicHashMap<std::string, int32_t> map;
  map.insert("x", 10);
  map.insert("y", 20);
  map.insert("z", 30);

  std::string concatenated;
  int32_t total = 0;
  map.for_each([&](const std::string& key, const int32_t& val) {
    concatenated += key;
    total += val;
  });
  EXPECT_EQ(total, 60);
  EXPECT_EQ(concatenated.size(), 3U);
}

TEST(AtomicHashMapTest, InsertRValueKey) {
  tianshu::base::AtomicHashMap<std::string, int32_t> map;
  std::string key = "movable_key";
  map.insert(std::move(key), 42);
  EXPECT_EQ(map.size(), 1U);
  EXPECT_TRUE(map.contains("movable_key"));
}

TEST(AtomicHashMapTest, SizeGrowsAndShrinks) {
  tianshu::base::AtomicHashMap<int32_t, int32_t> map;
  for (int32_t i = 0; i < 10; ++i) {
    map.insert(i, i);
  }
  EXPECT_EQ(map.size(), 10U);
  for (int32_t i = 0; i < 5; ++i) {
    map.erase(i);
  }
  EXPECT_EQ(map.size(), 5U);
  for (int32_t i = 0; i < 5; ++i) {
    EXPECT_FALSE(map.contains(i));
  }
  for (int32_t i = 5; i < 10; ++i) {
    EXPECT_TRUE(map.contains(i));
  }
}

TEST(AtomicHashMapTest, ConcurrentInsertFind) {
  constexpr int kThreads = 4;
  constexpr int kIterations = 5000;
  tianshu::base::AtomicHashMap<int32_t, int32_t> map;

  std::thread threads[kThreads];
  for (int t = 0; t < kThreads; ++t) {
    threads[t] = std::thread([&, t]() {
      const int32_t base = t * kIterations;
      for (int i = 0; i < kIterations; ++i) {
        map.insert(base + i, i);
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }

  EXPECT_EQ(map.size(), static_cast<std::size_t>(kThreads * kIterations));

  for (int t = 0; t < kThreads; ++t) {
    const int32_t base = t * kIterations;
    for (int i = 0; i < kIterations; ++i) {
      auto val = map.find(base + i);
      ASSERT_TRUE(val.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      EXPECT_EQ(*val, i);
    }
  }
}

TEST(AtomicHashMapTest, StringKeys) {
  tianshu::base::AtomicHashMap<std::string, int32_t> map;
  map.insert("alpha", 1);
  map.insert("beta", 2);
  map.insert("gamma", 3);
  EXPECT_EQ(map.size(), 3U);
  auto val = map.find("beta");
  ASSERT_TRUE(val.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_EQ(*val, 2);
  EXPECT_TRUE(map.erase("beta"));
  EXPECT_EQ(map.size(), 2U);
  EXPECT_FALSE(map.contains("beta"));
}

}  // namespace
