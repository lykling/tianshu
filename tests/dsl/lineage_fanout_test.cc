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

// Regression tests: lineage integrity when one channel feeds MULTIPLE
// consumers. Root-caused from full_chain_demo crashing with a double
// free / invalid pointer inside LineageHop's destructor (gcc 15 Debug,
// 2026-09-22): single-consumer flows stay healthy, flows where one
// channel feeds a join AND a sink (or a feedback from() bridge) corrupt
// lineage memory. The shapes below narrow the trigger progressively.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "tianshu/core/component.h"
#include "tianshu/core/lineage.h"
#include "tianshu/core/message_traits.h"
#include "tianshu/dsl/dsl_runtime.h"
#include "tianshu/dsl/flow.h"

// NOLINTNEXTLINE(misc-use-internal-linkage)  // traits must precede template use
struct FanFast {
  std::uint64_t t;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct FanSlow {
  std::uint64_t t;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct FanMid {
  std::uint64_t t;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct FanJoined {
  std::uint64_t t;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct FanCtrl {
  std::uint64_t t;
  double accel;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct FanState {
  std::uint64_t t;
  double v;
};

TIANSHU_TRAITS_POD(FanFast, "fan.FanFast");
TIANSHU_TRAITS_POD(FanSlow, "fan.FanSlow");
TIANSHU_TRAITS_POD(FanMid, "fan.FanMid");
TIANSHU_TRAITS_POD(FanJoined, "fan.FanJoined");
TIANSHU_TRAITS_POD(FanCtrl, "fan.FanCtrl");
TIANSHU_TRAITS_POD(FanState, "fan.FanState");

namespace {

using tianshu::core::Lineage;

// Minimal plant box for the feedback shape: consumes commands, publishes
// integrated state; init() publishes the power-on report so the loop
// bootstraps (same pattern as full_chain_demo's chassis).
class FanPlant final : public tianshu::core::Component<FanCtrl, FanState> {
 public:
  explicit FanPlant(std::string name) : Component(std::move(name)) {}

  bool init() override {
    publish(FanState{.t = 0, .v = 0.0});
    return true;
  }

 protected:
  void proc(const FanCtrl& cmd) override {
    v_ += cmd.accel;
    publish(FanState{.t = cmd.t, .v = v_});
  }

  [[nodiscard]] std::string_view out_channel() const override { return {}; }

 private:
  double v_{0.0};
};

struct FanObs {
  std::size_t hits{0};
  std::string last_lineage;
  std::size_t max_branches{0};

  void note(const Lineage& lin) {
    ++hits;
    last_lineage = lin.describe();
    max_branches = std::max(max_branches, lin.branches().size());
  }
};

}  // namespace

TIANSHU_REGISTER_COMPONENT(FanPlant, "fan.plant")

// Control shape: linear chain, every channel single-consumer. This is
// dsl_demo's shape and is expected to stay healthy.
TEST(LineageFanoutTest, LinearSingleConsumerSurvives) {
  FanObs obs;
  tianshu::dsl::FlowBuilder b("fan_linear");
  auto fast = b.source<FanFast>("fast", std::chrono::milliseconds(5),
                                [](std::uint64_t t) { return FanFast{.t = t}; });
  fast.map<FanMid>([](const FanFast& f) { return FanMid{.t = f.t}; })
      .sink([&obs](const FanMid&, const Lineage& lin) { obs.note(lin); });

  const auto flow = b.build();
  tianshu::dsl::FlowRuntime rt;
  rt.run_for(flow, std::chrono::milliseconds(300));

  EXPECT_GT(obs.hits, 0U);
  EXPECT_FALSE(obs.last_lineage.empty());
}

// One channel (the map output) feeds BOTH a join and a sink.
TEST(LineageFanoutTest, JoinAndSinkShareOneChannel) {
  FanObs mid_obs;
  FanObs joined_obs;
  tianshu::dsl::FlowBuilder b("fan_join_sink");
  auto fast = b.source<FanFast>("fast", std::chrono::milliseconds(5),
                                [](std::uint64_t t) { return FanFast{.t = t}; });
  auto slow = b.source<FanSlow>("slow", std::chrono::milliseconds(50),
                                [](std::uint64_t t) { return FanSlow{.t = t}; });
  auto mid = fast.map<FanMid>([](const FanFast& f) { return FanMid{.t = f.t}; });
  b.join<FanMid, FanSlow, FanJoined>(mid, slow, [](const FanMid& m, const FanSlow& s) {
     return FanJoined{.t = m.t + s.t};
   }).sink([&joined_obs](const FanJoined&, const Lineage& lin) { joined_obs.note(lin); });
  mid.sink([&mid_obs](const FanMid&, const Lineage& lin) { mid_obs.note(lin); });

  const auto flow = b.build();
  tianshu::dsl::FlowRuntime rt;
  rt.run_for(flow, std::chrono::milliseconds(300));

  EXPECT_GT(mid_obs.hits, 0U);
  EXPECT_GT(joined_obs.hits, 0U);
  EXPECT_FALSE(mid_obs.last_lineage.empty());
  EXPECT_FALSE(joined_obs.last_lineage.empty());
}

// full_chain_demo's loop shape: the state channel written by a from()
// bridge feeds a join (feedback) AND a sink; the lineage chains grow
// long enough to overflow SmallVec's inline hops (heap path).
TEST(LineageFanoutTest, FeedbackBridgeStateHasTwoConsumers) {
  FanObs state_obs;
  tianshu::dsl::FlowBuilder b("fan_feedback");
  auto fast = b.source<FanFast>("fast", std::chrono::milliseconds(5),
                                [](std::uint64_t t) { return FanFast{.t = t}; });
  auto state_port = b.tap<FanState>("state");
  auto ctrl = b.join<FanFast, FanState, FanCtrl>(
      fast, state_port,
      [](const FanFast& f, const FanState& s) { return FanCtrl{.t = f.t, .accel = 0.1 + s.v}; });
  auto plant = b.from<FanCtrl, FanState>("fan.plant", ctrl, "state");
  plant.sink([&state_obs](const FanState&, const Lineage& lin) { state_obs.note(lin); });

  const auto flow = b.build();
  tianshu::dsl::FlowRuntime rt;
  rt.run_for(flow, std::chrono::milliseconds(300));

  EXPECT_GT(state_obs.hits, 0U);
  EXPECT_FALSE(state_obs.last_lineage.empty());
}
