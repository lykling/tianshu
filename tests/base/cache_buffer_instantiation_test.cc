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

// CacheBuffer<T> template instantiation coverage: DataVisitor's
// T0..T3 visitor shapes instantiate the buffer for several payloads;
// this exercises fill/fill_bytes/observe/empty/capacity on each so the
// per-instantiation lines stop counting as uncovered. (Plain TESTs,
// not TYPED_TEST — GCC 15's -Wtemplate-body trips on the suite macro
// inside namespaces.)

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "tianshu/base/cache_buffer.h"

namespace {

struct Pose2 {
  double x{0};
  double y{0};
};
struct Blob {
  std::string tag;
  std::vector<std::uint8_t> bytes;
};

template <typename T>
void exercise_full_lifecycle() {
  tianshu::base::CacheBuffer<T> buf(3);
  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(buf.capacity(), 3U);
  EXPECT_EQ(buf.observe(), nullptr);

  const T first{};
  buf.fill(first);
  buf.fill_bytes(&first, sizeof(first));
  const T* observed = buf.observe();
  ASSERT_NE(observed, nullptr);
  EXPECT_NE(buf.try_fetch(), nullptr);
  EXPECT_FALSE(buf.empty());
}

TEST(CacheBufferInstantiation, Uint8Payload) { exercise_full_lifecycle<std::uint8_t>(); }
TEST(CacheBufferInstantiation, Uint64Payload) { exercise_full_lifecycle<std::uint64_t>(); }
TEST(CacheBufferInstantiation, StructPayload) { exercise_full_lifecycle<Pose2>(); }
TEST(CacheBufferInstantiation, StringyPayload) { exercise_full_lifecycle<Blob>(); }

}  // namespace
