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

// SmallVec unit tests (ADR-0030 D8 L2b): the inline fast path, the
// heap-overflow path, and every copy/move combination across the
// boundary — Lineage's hot-path correctness rests on these.

#include "tianshu/base/small_vector.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using Vec = tianshu::base::SmallVec<std::string, 2>;

TEST(SmallVecTest, InlineBasics) {
  Vec v;
  EXPECT_TRUE(v.empty());
  v.push_back("a");
  v.push_back("b");
  ASSERT_EQ(v.size(), 2U);
  EXPECT_EQ(v.front(), "a");
  EXPECT_EQ(v[1], "b");
  EXPECT_FALSE(v.empty());
}

TEST(SmallVecTest, OverflowGrowsAndKeepsOrder) {
  Vec v;
  for (int i = 0; i < 7; ++i) {
    v.push_back("item" + std::to_string(i));
  }
  ASSERT_EQ(v.size(), 7U);
  for (int i = 0; i < 7; ++i) {
    EXPECT_EQ(v[static_cast<std::size_t>(i)], "item" + std::to_string(i));
  }
}

TEST(SmallVecTest, IterationMatchesIndexing) {
  Vec v;
  v.push_back("x");
  v.push_back("y");
  v.push_back("z");
  std::vector<std::string> seen(v.begin(), v.end());
  ASSERT_EQ(seen.size(), 3U);
  EXPECT_EQ(seen[0], "x");
  EXPECT_EQ(seen[2], "z");
}

TEST(SmallVecTest, CopyInlineAndOverflow) {
  Vec a;
  a.push_back("one");
  Vec inline_copy = a;
  ASSERT_EQ(inline_copy.size(), 1U);
  EXPECT_EQ(inline_copy[0], "one");

  a.push_back("two");
  a.push_back("three");  // overflow
  Vec overflow_copy = a;
  ASSERT_EQ(overflow_copy.size(), 3U);
  EXPECT_EQ(overflow_copy[2], "three");

  overflow_copy[0] = "mutated";
  EXPECT_EQ(a[0], "one");  // deep copy: source untouched
}

TEST(SmallVecTest, MoveInlineAndOverflow) {
  Vec a;
  a.push_back("one");
  Vec moved_inline = std::move(a);
  ASSERT_EQ(moved_inline.size(), 1U);
  EXPECT_EQ(moved_inline[0], "one");
  // Reuse after move is the documented contract; exercising it here.
  // NOLINTBEGIN(bugprone-use-after-move)
  EXPECT_TRUE(a.empty());
  a.push_back("fresh");
  EXPECT_EQ(a.size(), 1U);
  EXPECT_EQ(a[0], "fresh");
  // NOLINTEND(bugprone-use-after-move)

  Vec b;
  b.push_back("1");
  b.push_back("2");
  b.push_back("3");
  b.push_back("4");  // heap side
  Vec moved_heap = std::move(b);
  ASSERT_EQ(moved_heap.size(), 4U);
  EXPECT_EQ(moved_heap[3], "4");
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_TRUE(b.empty());
}

TEST(SmallVecTest, CopyAssignmentAcrossBoundary) {
  Vec a;
  a.push_back("deep1");
  a.push_back("deep2");
  a.push_back("deep3");
  Vec b;
  b.push_back("shallow");
  b = a;
  ASSERT_EQ(b.size(), 3U);
  EXPECT_EQ(b[1], "deep2");

  Vec c;
  c.push_back("x");
  c.push_back("y");
  c.push_back("z");
  c.push_back("w");
  c.push_back("v");  // heap
  b = c;             // heap -> assigned over heap
  ASSERT_EQ(b.size(), 5U);
  EXPECT_EQ(b[4], "v");
}

TEST(SmallVecTest, MoveAssignmentSelfSafety) {
  Vec a;
  a.push_back("solo");
  Vec& alias = a;
  a = std::move(alias);  // self-move: must stay valid
  EXPECT_EQ(a.size(), 1U);
  EXPECT_EQ(a[0], "solo");
}

}  // namespace
