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

// Unit tests for DataDispatcher / DataNotifier / DataVisitor (L4-CORE-5/6/7).

#include "tianshu/core/data_visitor.h"

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "tianshu/base/cache_buffer.h"
#include "tianshu/core/data_dispatcher.h"
#include "tianshu/core/data_notifier.h"

namespace {

// Runtime (non-constexpr) FNV calls cannot be constexpr globals; helpers
// below keep test lines short without static-storage throw risk.
tianshu::core::ChannelId id_a() { return tianshu::core::channel_id_for("/test/a"); }
tianshu::core::ChannelId id_b() { return tianshu::core::channel_id_for("/test/b"); }

struct Pose {
  double x;
  double y;
};

struct Img {
  std::uint32_t w;
  std::uint32_t h;
};

TEST(DataDispatcherTest, FillsRegisteredBufferAndNotifies) {
  auto& dispatcher = tianshu::core::DataDispatcher::instance();
  tianshu::base::CacheBuffer<Pose> buffer(4);
  int notified = 0;
  dispatcher.add_buffer(id_a(), &buffer, [&notified] { ++notified; }, &buffer);

  const Pose msg{.x = 1.5, .y = -2.5};
  dispatcher.dispatch(id_a(), &msg, sizeof(msg));

  EXPECT_EQ(notified, 1);
  const Pose* got = buffer.try_fetch();
  ASSERT_NE(got, nullptr);
  EXPECT_DOUBLE_EQ(got->x, 1.5);
  EXPECT_DOUBLE_EQ(got->y, -2.5);

  tianshu::core::DataDispatcher::instance().remove_owner(&buffer);
}

TEST(DataDispatcherTest, MultipleSinksSameChannel) {
  auto& dispatcher = tianshu::core::DataDispatcher::instance();
  tianshu::base::CacheBuffer<Pose> b1(2);
  tianshu::base::CacheBuffer<Pose> b2(2);
  int n1 = 0;
  int n2 = 0;
  dispatcher.add_buffer(id_b(), &b1, [&n1] { ++n1; }, &b1);
  dispatcher.add_buffer(id_b(), &b2, [&n2] { ++n2; }, &b2);

  const Pose msg{.x = 0, .y = 0};
  dispatcher.dispatch(id_b(), &msg, sizeof(msg));

  EXPECT_EQ(n1, 1);
  EXPECT_EQ(n2, 1);
  EXPECT_NE(b1.try_fetch(), nullptr);
  EXPECT_NE(b2.try_fetch(), nullptr);

  tianshu::core::DataDispatcher::instance().remove_owner(&b1);
  tianshu::core::DataDispatcher::instance().remove_owner(&b2);
}

TEST(DataDispatcherTest, NoSinksIsNoop) {
  auto& dispatcher = tianshu::core::DataDispatcher::instance();
  const Pose msg{.x = 0, .y = 0};
  dispatcher.dispatch(tianshu::core::channel_id_for("/test/none"), &msg, sizeof(msg));
  SUCCEED();
}

TEST(DataDispatcherTest, RemoveOwnerStopsDelivery) {
  auto& dispatcher = tianshu::core::DataDispatcher::instance();
  tianshu::base::CacheBuffer<Pose> buffer(2);
  int notified = 0;
  dispatcher.add_buffer(id_a(), &buffer, [&notified] { ++notified; }, &buffer);

  dispatcher.remove_owner(&buffer);
  const Pose msg{.x = 9, .y = 9};
  dispatcher.dispatch(id_a(), &msg, sizeof(msg));

  EXPECT_EQ(notified, 0);
  EXPECT_EQ(buffer.try_fetch(), nullptr);
}

TEST(DataVisitorTest, SingleInputFusesOnData) {
  int fused = 0;
  {
    tianshu::core::DataVisitor<Pose> visitor("/test/v1", 4, [&fused] { ++fused; });

    auto& dispatcher = tianshu::core::DataDispatcher::instance();
    const Pose msg{.x = 3, .y = 4};
    dispatcher.dispatch(tianshu::core::channel_id_for("/test/v1"), &msg, sizeof(msg));
    EXPECT_EQ(fused, 1);

    const Pose* got = visitor.try_fetch_0();
    ASSERT_NE(got, nullptr);
    EXPECT_DOUBLE_EQ(got->x, 3);
  }
}

TEST(DataVisitorTest, TwoInputsFuseOnlyWhenBothReady) {
  auto& dispatcher = tianshu::core::DataDispatcher::instance();
  int fused = 0;
  {
    tianshu::core::DataVisitor<Pose, Img> visitor("/test/v2a", "/test/v2b", 4,
                                                  [&fused] { ++fused; });

    const Pose pose{.x = 1, .y = 2};
    dispatcher.dispatch(tianshu::core::channel_id_for("/test/v2a"), &pose, sizeof(pose));
    EXPECT_EQ(fused, 0);

    const Img img{.w = 640, .h = 480};
    dispatcher.dispatch(tianshu::core::channel_id_for("/test/v2b"), &img, sizeof(img));
    EXPECT_EQ(fused, 1);

    const Pose* p = visitor.try_fetch_0();
    const Img* i = visitor.try_fetch_1();
    ASSERT_NE(p, nullptr);
    ASSERT_NE(i, nullptr);
    EXPECT_EQ(i->w, 640U);
  }
}

TEST(DataVisitorTest, TeardownUnregistersBuffers) {
  auto& dispatcher = tianshu::core::DataDispatcher::instance();
  {
    int fused = 0;
    const tianshu::core::DataVisitor<Pose> visitor("/test/v3", 2, [&fused] { ++fused; });
  }
  const Pose msg{.x = 0, .y = 0};
  dispatcher.dispatch(tianshu::core::channel_id_for("/test/v3"), &msg, sizeof(msg));
  SUCCEED();
}

TEST(DataVisitorTest, ChannelIdStableAndDistinct) {
  EXPECT_EQ(tianshu::core::channel_id_for("/test/a"), tianshu::core::channel_id_for("/test/a"));
  EXPECT_NE(tianshu::core::channel_id_for("/test/a"), tianshu::core::channel_id_for("/test/b"));
  EXPECT_NE(tianshu::core::channel_id_for(""), tianshu::core::channel_id_for("/test/a"));
}

TEST(DataNotifierTest, FiresRegisteredCallbacks) {
  auto& notifier = tianshu::core::DataNotifier::instance();
  int fired = 0;
  notifier.add_notifier(id_a(), [&fired] { ++fired; }, &fired);

  EXPECT_EQ(notifier.notify(id_a()), 1U);
  EXPECT_EQ(fired, 1);
  EXPECT_EQ(notifier.notify(id_b()), 0U);
  EXPECT_EQ(fired, 1);
}

TEST(DataNotifierTest, RemoveOwnerStopsFiring) {
  auto& notifier = tianshu::core::DataNotifier::instance();
  int fired = 0;
  notifier.add_notifier(id_b(), [&fired] { ++fired; }, &fired);
  notifier.remove_owner(&fired);
  EXPECT_EQ(notifier.notify(id_b()), 0U);
  EXPECT_EQ(fired, 0);
}

}  // namespace
