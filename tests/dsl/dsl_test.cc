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

// Unit tests for DSL v0 declarations + runtime + lineage (ADR-0021/0022).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "tianshu/core/lineage.h"
#include "tianshu/core/message_traits.h"
#include "tianshu/dsl/dsl_runtime.h"
#include "tianshu/dsl/flow.h"

// NOLINTNEXTLINE(misc-use-internal-linkage)  // traits must precede template use
struct TickMsg {
  std::uint64_t tick;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct DoubledMsg {
  std::uint64_t tick;
  double value;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct ScaledMsg {
  std::uint64_t tick;
  double value;
};

// Traits must precede any template use (MessageConcept instantiation).
TIANSHU_TRAITS_POD(TickMsg, "t.TickMsg");
TIANSHU_TRAITS_POD(DoubledMsg, "t.DoubledMsg");
TIANSHU_TRAITS_POD(ScaledMsg, "t.ScaledMsg");

namespace {

TEST(LineageTest, RootedDescribe) {
  const auto lin = tianshu::core::Lineage::rooted("a/b", 7);
  EXPECT_EQ(lin.describe(), "a/b#7");
  EXPECT_EQ(lin.root().channel, "a/b");
  EXPECT_EQ(lin.root().seq, 7U);
  EXPECT_TRUE(lin.hops().empty());
}

TEST(LineageTest, HopChainFormat) {
  auto lin = tianshu::core::Lineage::rooted("a/b", 7);
  lin.add_hop({.channel = "x", .seq = 1});
  lin.add_hop({.channel = "y", .seq = 2});
  EXPECT_EQ(lin.describe(), "a/b#7 -> x#1 -> y#2");
  ASSERT_EQ(lin.hops().size(), 2U);
  EXPECT_EQ(lin.hops()[1].channel, "y");
}

TEST(FlowBuilderTest, RecordsGraphShape) {
  const auto flow = tianshu::dsl::FlowBuilder("demo")
                        .source<TickMsg>("ticks", std::chrono::milliseconds(10),
                                         [](std::uint64_t t) { return TickMsg{.tick = t}; })
                        .map<DoubledMsg>([](const TickMsg& in) {
                          return DoubledMsg{.tick = in.tick, .value = 0};
                        })
                        .map<ScaledMsg>([](const DoubledMsg& in) {
                          return ScaledMsg{.tick = in.tick, .value = 0};
                        })
                        .sink([](const ScaledMsg&, const tianshu::core::Lineage&) {})
                        .build();

  ASSERT_EQ(flow.sources().size(), 1U);
  EXPECT_EQ(flow.sources()[0].channel, "demo/ticks");
  EXPECT_EQ(flow.sources()[0].type_name, "t.TickMsg");
  EXPECT_EQ(flow.sources()[0].interval, std::chrono::milliseconds(10));

  ASSERT_EQ(flow.maps().size(), 2U);
  EXPECT_EQ(flow.maps()[0].in_channel, "demo/ticks");
  EXPECT_EQ(flow.maps()[0].out_channel, "demo/~0");
  EXPECT_EQ(flow.maps()[1].in_channel, "demo/~0");
  EXPECT_EQ(flow.maps()[1].out_channel, "demo/~1");

  ASSERT_EQ(flow.sinks().size(), 1U);
  EXPECT_EQ(flow.sinks()[0].channel, "demo/~1");

  EXPECT_EQ(flow.describe(),
            "flow demo: src[demo/ticks] map[demo/ticks -> demo/~0] map[demo/~0 -> demo/~1] "
            "sink[demo/~1]");
}

TEST(DslRuntimeTest, CascadeValuesAndLineage) {
  struct Received {
    std::uint64_t tick;
    double value;
    std::string lineage;
  };
  std::vector<Received> received;

  const auto flow =
      tianshu::dsl::FlowBuilder("demo")
          .source<TickMsg>("ticks", std::chrono::milliseconds(5),
                           [](std::uint64_t t) { return TickMsg{.tick = t}; })
          .map<DoubledMsg>([](const TickMsg& in) {
            return DoubledMsg{.tick = in.tick, .value = static_cast<double>(in.tick) * 2};
          })
          .map<ScaledMsg>([](const DoubledMsg& in) {
            return ScaledMsg{.tick = in.tick, .value = in.value + 0.5};
          })
          .sink([&](const ScaledMsg& msg, const tianshu::core::Lineage& lin) {
            received.push_back(
                Received{.tick = msg.tick, .value = msg.value, .lineage = lin.describe()});
          })
          .build();

  tianshu::dsl::FlowRuntime runtime;
  runtime.run_for(flow, std::chrono::milliseconds(120));

  ASSERT_GE(received.size(), 10U) << "expected >= 10 ticks in 120ms, got " << received.size();

  for (std::size_t i = 0; i < received.size(); ++i) {
    EXPECT_EQ(received[i].tick, i) << "ticks must be contiguous from 0";
    EXPECT_DOUBLE_EQ(received[i].value, (static_cast<double>(i) * 2) + 0.5);
    // Single source + synchronous cascade: every seq equals the tick.
    EXPECT_EQ(received[i].lineage, "demo/ticks#" + std::to_string(i) + " -> demo/~0#" +
                                       std::to_string(i) + " -> demo/~1#" + std::to_string(i))
        << "lineage chain mismatch at tick " << i;
  }
}

TEST(DslRuntimeTest, SlaAnnotationAcceptedButIgnored) {
  const auto flow = tianshu::dsl::FlowBuilder("sla")
                        .with_sla("period=10ms deadline=5ms")
                        .source<TickMsg>("t", std::chrono::milliseconds(5),
                                         [](std::uint64_t t) { return TickMsg{.tick = t}; })
                        .sink([](const TickMsg&, const tianshu::core::Lineage&) {})
                        .build();
  EXPECT_EQ(flow.sources().size(), 1U);

  tianshu::dsl::FlowRuntime runtime;
  runtime.run_for(flow, std::chrono::milliseconds(20));
  SUCCEED();
}

}  // namespace
