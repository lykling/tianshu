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

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "tianshu/core/lineage.h"
#include "tianshu/core/message_traits.h"
#include "tianshu/dsl/dsl_runtime.h"
#include "tianshu/dsl/flow.h"
#include "tianshu/sla/sla_analyzer.h"

// NOLINTNEXTLINE(misc-use-internal-linkage)  // traits must precede template use
struct FbTick {
  std::uint64_t tick{0};
};
// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct FbOut {
  std::uint64_t value{0};
};

TIANSHU_TRAITS_POD(FbTick, "fb.TickMsg");
TIANSHU_TRAITS_POD(FbOut, "fb.OutMsg");

// NOLINTNEXTLINE(misc-use-internal-linkage)  // must be nameable by the registration macro
void declare_lite(tianshu::dsl::FlowBuilder& b) {
  b.source<FbTick>("lite_ticks", std::chrono::milliseconds(5), [](std::uint64_t t) {
     return FbTick{.tick = t};
   }).sink([](const FbTick&, const tianshu::core::Lineage&) {});
}
REGISTER_TRACEABLE_FLOW("fb_lite", declare_lite)

namespace {

using tianshu::core::Lineage;
using tianshu::dsl::FlowBuilder;
using tianshu::dsl::FlowRuntime;

// ---------------------------------------------------------------------------
// Declaration + load-time validation
// ---------------------------------------------------------------------------

TEST(FlowFallbackTest, DeclaresAndCarriesFallbackName) {
  FlowBuilder b("fb_ok");
  auto chain = b.source<FbTick>("ticks", std::chrono::milliseconds(5),
                                [](std::uint64_t t) { return FbTick{.tick = t}; });
  chain.with_fallback("fb_lite");
  const auto flow = chain.build();
  EXPECT_EQ(flow.fallback_flow(), "fb_lite");
}

TEST(FlowFallbackTest, NoFallbackByDefault) {
  FlowBuilder b("fb_plain");
  auto chain = b.source<FbTick>("ticks", std::chrono::milliseconds(5),
                                [](std::uint64_t t) { return FbTick{.tick = t}; });
  EXPECT_EQ(chain.build().fallback_flow(), "");
}

TEST(FlowFallbackTest, UnknownFallbackFailsFastAtBuild) {
  FlowBuilder b("fb_bad");
  auto chain = b.source<FbTick>("ticks", std::chrono::milliseconds(5),
                                [](std::uint64_t t) { return FbTick{.tick = t}; });
  chain.with_fallback("no_such_flow");
  EXPECT_THROW(static_cast<void>(chain.build()), std::invalid_argument);
}

TEST(FlowFallbackTest, SelfFallbackRejected) {
  FlowBuilder b("fb_self");
  auto chain = b.source<FbTick>("ticks", std::chrono::milliseconds(5),
                                [](std::uint64_t t) { return FbTick{.tick = t}; });
  chain.with_fallback("fb_self");
  EXPECT_THROW(static_cast<void>(chain.build()), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Runtime degradation detection (ADR-0031 v0)
// ---------------------------------------------------------------------------

TEST(FlowFallbackRuntimeTest, PersistentDeadlineMissFiresDegradationEvent) {
  FlowBuilder b("fb_slow");
  auto out = b.source<FbTick>("ticks", std::chrono::milliseconds(5), [](std::uint64_t t) {
                return FbTick{.tick = t};
              }).map<FbOut>([](const FbTick& in) {
    std::this_thread::sleep_for(std::chrono::milliseconds(4));  // blows the 1ms deadline
    return FbOut{.value = in.tick};
  });
  out.with_sla(tianshu::sla::Sla{.deadline = std::chrono::microseconds(1000)});
  out.with_fallback("fb_lite");
  out.sink([](const FbOut&, const Lineage&) {});
  const auto flow = b.build();

  FlowRuntime rt;
  rt.run_for(flow, std::chrono::milliseconds(300));
  const auto state = rt.fallback_state();
  EXPECT_EQ(state.declared, "fb_lite");
  EXPECT_GE(state.events, 1U);
  EXPECT_FALSE(state.last_endpoint.empty());
  EXPECT_GT(state.last_miss_count, 0U);
}

TEST(FlowFallbackRuntimeTest, HealthyFlowFiresNoEvent) {
  FlowBuilder b("fb_fast");
  auto out = b.source<FbTick>("ticks", std::chrono::milliseconds(10), [](std::uint64_t t) {
                return FbTick{.tick = t};
              }).map<FbOut>([](const FbTick& in) { return FbOut{.value = in.tick}; });
  out.with_sla(tianshu::sla::Sla{.deadline = std::chrono::microseconds(50000)});
  out.with_fallback("fb_lite");
  out.sink([](const FbOut&, const Lineage&) {});
  const auto flow = b.build();

  FlowRuntime rt;
  rt.run_for(flow, std::chrono::milliseconds(200));
  const auto state = rt.fallback_state();
  EXPECT_EQ(state.declared, "fb_lite");
  EXPECT_EQ(state.events, 0U);
}

TEST(FlowFallbackRuntimeTest, NoFallbackDeclaredMeansNoWatching) {
  FlowBuilder b("fb_nofb");
  auto out = b.source<FbTick>("ticks", std::chrono::milliseconds(10), [](std::uint64_t t) {
                return FbTick{.tick = t};
              }).map<FbOut>([](const FbTick& in) { return FbOut{.value = in.tick}; });
  out.with_sla(tianshu::sla::Sla{.deadline = std::chrono::microseconds(50000)});
  out.sink([](const FbOut&, const Lineage&) {});
  const auto flow = b.build();

  FlowRuntime rt;
  rt.run_for(flow, std::chrono::milliseconds(100));
  const auto state = rt.fallback_state();
  EXPECT_TRUE(state.declared.empty());
  EXPECT_EQ(state.events, 0U);
}

}  // namespace
