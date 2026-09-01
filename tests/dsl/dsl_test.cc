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
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "tianshu/core/component.h"
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

// NOLINTNEXTLINE(misc-use-internal-linkage)  // traits must precede template use
struct FastMsg {
  std::uint64_t t;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct SlowMsg {
  std::uint64_t t;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct FusedMsg {
  std::uint64_t fast_t;
  std::uint64_t slow_t;
};

TIANSHU_TRAITS_POD(FastMsg, "t.FastMsg");
TIANSHU_TRAITS_POD(SlowMsg, "t.SlowMsg");
TIANSHU_TRAITS_POD(FusedMsg, "t.FusedMsg");

// NOLINTNEXTLINE(misc-use-internal-linkage)  // traits must precede template use
struct LoopMeas {
  std::uint64_t t;
  double hazard;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // traits must precede template use
struct LoopPlan {
  std::uint64_t t;
  double target;
  double v;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // traits must precede template use
struct LoopCmd {
  std::uint64_t t;
  double u;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // traits must precede template use
struct LoopState {
  std::uint64_t t;
  double v;
};

TIANSHU_TRAITS_POD(LoopMeas, "t.LoopMeas");
TIANSHU_TRAITS_POD(LoopPlan, "t.LoopPlan");
TIANSHU_TRAITS_POD(LoopCmd, "t.LoopCmd");
TIANSHU_TRAITS_POD(LoopState, "t.LoopState");

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

TEST(LineageTest, JoinMergesBranches) {
  auto la = tianshu::core::Lineage::rooted("a", 1);
  la.add_hop({.channel = "x", .seq = 1});
  auto lb = tianshu::core::Lineage::rooted("b", 5);

  auto merged = la;
  merged.merge(lb);
  merged.add_hop({.channel = "j", .seq = 0});

  EXPECT_EQ(merged.branches().size(), 2U);
  EXPECT_EQ(merged.describe(), "a#1 -> x#1 -> j#0 | b#5 -> j#0");
}

TEST(DslRuntimeTest, TwoSinksOnOneChannelBothSeeEverything) {
  std::vector<std::string> sink_a;
  std::vector<std::string> sink_b;
  const auto flow = tianshu::dsl::FlowBuilder("dual")
                        .source<TickMsg>("ticks", std::chrono::milliseconds(5),
                                         [](std::uint64_t t) { return TickMsg{.tick = t}; })
                        .sink([&](const TickMsg&, const tianshu::core::Lineage& lin) {
                          sink_a.push_back(lin.describe());
                        })
                        .sink([&](const TickMsg&, const tianshu::core::Lineage& lin) {
                          sink_b.push_back(lin.describe());
                        })
                        .build();

  tianshu::dsl::FlowRuntime runtime;
  runtime.run_for(flow, std::chrono::milliseconds(120));

  // Per-consumer mailboxes: both sinks see EVERY message with its full
  // lineage (the v0 side FIFO would have interleaved steals here).
  ASSERT_FALSE(sink_a.empty());
  ASSERT_EQ(sink_a.size(), sink_b.size());
  for (std::size_t i = 0; i < sink_a.size(); ++i) {
    EXPECT_EQ(sink_a[i], sink_b[i]);
    EXPECT_FALSE(sink_a[i].empty());
  }
}

TEST(DslRuntimeTest, JoinFusesAllLatestWithMergedLineage) {
  struct Sample {
    std::uint64_t fast_root;
    std::uint64_t slow_root;
    std::string lineage;
  };
  std::vector<Sample> samples;

  tianshu::dsl::FlowBuilder builder("fused");
  auto fast = builder.source<FastMsg>("fast", std::chrono::milliseconds(5),
                                      [](std::uint64_t t) { return FastMsg{.t = t}; });
  auto slow = builder.source<SlowMsg>("slow", std::chrono::milliseconds(20),
                                      [](std::uint64_t t) { return SlowMsg{.t = t}; });
  auto joined = builder.join<FastMsg, SlowMsg, FusedMsg>(
      fast, slow,
      [](const FastMsg& f, const SlowMsg& s) { return FusedMsg{.fast_t = f.t, .slow_t = s.t}; });
  joined.sink([&](const FusedMsg&, const tianshu::core::Lineage& lin) {
    if (samples.size() < 32) {
      samples.push_back({.fast_root = lin.branches()[0].root.seq,
                         .slow_root = lin.branches()[1].root.seq,
                         .lineage = lin.describe()});
    }
  });
  const auto flow = builder.build();

  EXPECT_NE(flow.describe().find("join["), std::string::npos);

  tianshu::dsl::FlowRuntime runtime;
  runtime.run_for(flow, std::chrono::milliseconds(300));

  ASSERT_FALSE(samples.empty());
  // Fused values pair a fast tick with a slow tick.
  for (const auto& s : samples) {
    EXPECT_TRUE(s.lineage.find(" | ") != std::string::npos);
    EXPECT_LE(s.slow_root, s.fast_root);
  }
  // Different source rates: the fast branch runs ahead of the slow one.
  bool diverged = false;
  for (const auto& s : samples) {
    if (s.fast_root > s.slow_root) {
      diverged = true;
      break;
    }
  }
  EXPECT_TRUE(diverged);
}

TEST(DslRuntimeTest, ClosedLoopFeedbackConvergesWithBoundedLineage) {
  struct Sample {
    double v;
    std::size_t branches;
    std::string lineage;
  };
  std::vector<Sample> samples;
  const auto plant_v = std::make_shared<double>(0.0);

  tianshu::dsl::FlowBuilder builder("loop");
  const auto meas =
      builder.source<LoopMeas>("meas", std::chrono::milliseconds(10),
                               [](std::uint64_t t) { return LoopMeas{.t = t, .hazard = 0.5}; });
  // Seed/heartbeat for the feedback channel (the loop needs one chassis
  // message before the plant has produced any).
  auto state_fb =
      builder.source<LoopState>("state", std::chrono::milliseconds(150),
                                [](std::uint64_t t) { return LoopState{.t = t, .v = 0.0}; });
  auto plan = builder.join<LoopMeas, LoopState, LoopPlan>(
      meas, state_fb, [](const LoopMeas& m, const LoopState& s) {
        return LoopPlan{.t = m.t, .target = 10.0 - m.hazard, .v = s.v};
      });
  auto cmd = plan.map<LoopCmd>(
      [](const LoopPlan& p) { return LoopCmd{.t = p.t, .u = 1.5 * (p.target - p.v)}; });
  const auto plant = cmd.map_to<LoopState>("state", [plant_v](const LoopCmd& c) {
    *plant_v += c.u * 0.1;
    return LoopState{.t = c.t, .v = *plant_v};
  });
  static_cast<void>(plant);
  state_fb.sink([&](const LoopState&, const tianshu::core::Lineage& lin) {
    if (samples.size() < 256) {
      samples.push_back(
          {.v = *plant_v, .branches = lin.branches().size(), .lineage = lin.describe()});
    }
  });

  const auto flow = builder.build();
  EXPECT_NE(flow.describe().find("map[loop/~1 -> loop/state]"), std::string::npos);

  tianshu::dsl::FlowRuntime runtime;
  runtime.run_for(flow, std::chrono::milliseconds(800));

  ASSERT_FALSE(samples.empty());
  for (const auto& s : samples) {
    EXPECT_LE(s.branches, tianshu::core::Lineage::kMaxBranches);
  }
  // The plant caught up with the planner's target (9.5 m/s).
  EXPECT_GT(samples.back().v, 6.0);
  EXPECT_LT(samples.back().v, 12.0);
  // The lineage carries the sensor root AND the loop-carrying hops.
  EXPECT_NE(samples.back().lineage.find("loop/meas#"), std::string::npos);
  bool has_loop_hops = false;
  for (const auto& s : samples) {
    if (s.lineage.find("loop/state#") != std::string::npos &&
        s.lineage.find("loop/~1#") != std::string::npos) {
      has_loop_hops = true;
      break;
    }
  }
  EXPECT_TRUE(has_loop_hops);
}

// NOLINTNEXTLINE(misc-use-internal-linkage)  // op contract uses instance invocation
class DoublerOp {
 public:
  static void on_init(tianshu::dsl::OpPub<FusedMsg>& pub) {
    pub.publish(FusedMsg{.fast_t = 0, .slow_t = 0});
  }

  static void handle(const TickMsg& in, tianshu::dsl::OpPub<FusedMsg>& pub) {
    pub.publish(FusedMsg{.fast_t = in.tick, .slow_t = in.tick * 2});
  }
};

TEST(DslRuntimeTest, OpLifecycleLineageSemantics) {
  std::vector<std::string> samples;

  tianshu::dsl::FlowBuilder builder("bx");
  auto ticks = builder.source<TickMsg>("ticks", std::chrono::milliseconds(5),
                                       [](std::uint64_t t) { return TickMsg{.tick = t}; });
  auto boxed = builder.op<TickMsg, FusedMsg>(ticks, "out", DoublerOp{});
  boxed.sink([&](const FusedMsg&, const tianshu::core::Lineage& lin) {
    if (samples.size() < 64) {
      samples.push_back(lin.describe());
    }
  });
  const auto flow = builder.build();
  EXPECT_NE(flow.describe().find("op[bx/ticks -> bx/out]"), std::string::npos);

  tianshu::dsl::FlowRuntime runtime;
  runtime.run_for(flow, std::chrono::milliseconds(60));

  ASSERT_GE(samples.size(), 2U);
  // on_init publication: rooted at the op's channel (source semantics).
  EXPECT_EQ(samples.front(), "bx/out#0");
  // handle publications: input lineage + output hop (map semantics).
  EXPECT_EQ(samples[1], "bx/ticks#0 -> bx/out#1");
}

// NOLINTNEXTLINE(misc-use-internal-linkage)  // op contract uses instance invocation
class CruiseOp {
 public:
  static void on_init(tianshu::dsl::OpPub<LoopState>& pub) {
    pub.publish(LoopState{.t = 0, .v = 0.0});
  }

  void handle(const LoopCmd& cmd, tianshu::dsl::OpPub<LoopState>& pub) {
    v_ += cmd.u * 0.1;
    pub.publish(LoopState{.t = cmd.t, .v = v_});
  }

 private:
  double v_{0.0};
};

TEST(DslRuntimeTest, OpBootstrapsFeedbackLoopWithoutSeedSource) {
  std::vector<double> speeds;
  std::vector<std::string> lineages;

  tianshu::dsl::FlowBuilder builder("bbox");
  auto meas = builder.source<LoopMeas>("meas", std::chrono::milliseconds(10), [](std::uint64_t t) {
    return LoopMeas{.t = t, .hazard = 0.5};
  });
  // Cycle-breaker port: planning references the state channel before the
  // box writing it exists — NO seed source anywhere.
  auto state_port = builder.tap<LoopState>("state");
  auto plan = builder.join<LoopMeas, LoopState, LoopPlan>(
      meas, state_port, [](const LoopMeas& m, const LoopState& s) {
        return LoopPlan{.t = m.t, .target = 10.0 - m.hazard, .v = s.v};
      });
  auto cmd = plan.map<LoopCmd>(
      [](const LoopPlan& p) { return LoopCmd{.t = p.t, .u = 1.5 * (p.target - p.v)}; });
  auto state = builder.op<LoopCmd, LoopState>(cmd, "state", CruiseOp{});
  state.sink([&](const LoopState& s, const tianshu::core::Lineage& lin) {
    if (speeds.size() < 256) {
      speeds.push_back(s.v);
      lineages.push_back(lin.describe());
    }
  });

  const auto flow = builder.build();
  // The graph has no source on the state channel — the op IS the chassis.
  EXPECT_EQ(flow.describe().find("src[bbox/state]"), std::string::npos);
  EXPECT_NE(flow.describe().find("op[bbox/~1 -> bbox/state]"), std::string::npos);

  tianshu::dsl::FlowRuntime runtime;
  runtime.run_for(flow, std::chrono::milliseconds(800));

  // Bootstrap: the very first state message is the box's on_init report.
  ASSERT_GE(speeds.size(), 2U);
  EXPECT_EQ(speeds.front(), 0.0);
  // Closed loop converges toward the 9.5 m/s target.
  EXPECT_GT(speeds.back(), 6.0);
  EXPECT_LT(speeds.back(), 12.0);
  // Lineage bounded and carrying the loop hops.
  for (const auto& lin : lineages) {
    EXPECT_TRUE(lin.find("bbox/meas#") != std::string::npos ||
                lin.find("bbox/state#") != std::string::npos);
  }
  bool has_loop_hops = false;
  for (const auto& lin : lineages) {
    if (lin.find("bbox/state#") != std::string::npos && lin.find("bbox/~1#") != std::string::npos) {
      has_loop_hops = true;
      break;
    }
  }
  EXPECT_TRUE(has_loop_hops);
}

namespace {
class TFromDriver final : public tianshu::core::TimerSourceComponent<FastMsg> {
 public:
  explicit TFromDriver(std::string name) : TimerSourceComponent(std::move(name)) {}

 protected:
  void proc() override { publish(FastMsg{.t = tick_++}); }

  [[nodiscard]] std::string_view out_channel() const override { return {}; }

 private:
  std::uint64_t tick_{0};
};

class TChassisComp final : public tianshu::core::Component<LoopCmd, LoopState> {
 public:
  explicit TChassisComp(std::string name) : Component(std::move(name)) {}

  bool init() override {
    publish(LoopState{.t = 0, .v = 0.0});
    return true;
  }

 protected:
  void proc(const LoopCmd& cmd) override {
    v_ += cmd.u * 0.1;
    publish(LoopState{.t = cmd.t, .v = v_});
  }

  [[nodiscard]] std::string_view out_channel() const override { return {}; }

 private:
  double v_{0.0};
};
}  // namespace

TIANSHU_REGISTER_COMPONENT(TFromDriver, "test.from.driver")
TIANSHU_REGISTER_COMPONENT(TChassisComp, "test.from.chassis")

TEST(FromReferenceTest, UnknownRegistrationYieldsInvalidChain) {
  tianshu::dsl::FlowBuilder builder("fm");
  const auto chain = builder.from<FastMsg>("no.such.driver", "out", std::chrono::milliseconds(5));
  EXPECT_FALSE(chain.valid());
}

TEST(FromReferenceTest, ShapeMismatchYieldsInvalidChain) {
  // Registered as TimerSourceComponent<FastMsg>; SlowMsg does not match.
  tianshu::dsl::FlowBuilder builder("fs");
  const auto chain = builder.from<SlowMsg>("test.from.driver", "out", std::chrono::milliseconds(5));
  EXPECT_FALSE(chain.valid());
}

TEST(FromReferenceTest, DriverProducesStreamWithRootedLineage) {
  std::vector<std::string> got;
  const auto flow = tianshu::dsl::FlowBuilder("fd")
                        .from<FastMsg>("test.from.driver", "radar", std::chrono::milliseconds(5))
                        .sink([&](const FastMsg&, const tianshu::core::Lineage& lin) {
                          if (got.size() < 32) {
                            got.push_back(lin.describe());
                          }
                        })
                        .build();
  EXPECT_NE(flow.describe().find("from[test.from.driver -> fd/radar]"), std::string::npos);

  tianshu::dsl::FlowRuntime runtime;
  runtime.run_for(flow, std::chrono::milliseconds(120));

  ASSERT_GE(got.size(), 2U);
  // Component output lineage = rooted at the (injected) output channel.
  EXPECT_EQ(got.front(), "fd/radar#0");
}

TEST(FromReferenceTest, ComponentClosesLoopViaInitBootstrap) {
  std::vector<double> speeds;
  tianshu::dsl::FlowBuilder builder("fc");
  auto meas = builder.source<LoopMeas>("meas", std::chrono::milliseconds(10), [](std::uint64_t t) {
    return LoopMeas{.t = t, .hazard = 0.5};
  });
  auto state_port = builder.tap<LoopState>("state");
  auto plan = builder.join<LoopMeas, LoopState, LoopPlan>(
      meas, state_port, [](const LoopMeas& m, const LoopState& s) {
        return LoopPlan{.t = m.t, .target = 10.0 - m.hazard, .v = s.v};
      });
  auto cmd = plan.map<LoopCmd>(
      [](const LoopPlan& p) { return LoopCmd{.t = p.t, .u = 1.5 * (p.target - p.v)}; });
  auto state = builder.from<LoopCmd, LoopState>("test.from.chassis", cmd, "state");
  ASSERT_TRUE(state.valid());
  state.sink([&](const LoopState& s, const tianshu::core::Lineage&) {
    if (speeds.size() < 256) {
      speeds.push_back(s.v);
    }
  });

  const auto flow = builder.build();
  EXPECT_NE(flow.describe().find("from[fc/~1 via test.from.chassis -> fc/state]"),
            std::string::npos);

  tianshu::dsl::FlowRuntime runtime;
  runtime.run_for(flow, std::chrono::milliseconds(800));

  // init() bootstrap: first message is the power-on report, then the loop
  // converges toward the 9.5 m/s target.
  ASSERT_GE(speeds.size(), 2U);
  EXPECT_EQ(speeds.front(), 0.0);
  EXPECT_GT(speeds.back(), 6.0);
  EXPECT_LT(speeds.back(), 12.0);
}

// ADR-0025 correction, live: a referenced component's proc publishes
// CARRY the input lineage (mailbox-paired), so the feedback loop unrolls
// across the component boundary — no rooted truncation.
TEST(FromReferenceTest, ComponentOutputDerivesLineageAndUnrollsLoop) {
  std::vector<std::string> states;

  tianshu::dsl::FlowBuilder builder("lu");
  auto meas = builder.source<LoopMeas>("meas", std::chrono::milliseconds(10), [](std::uint64_t t) {
    return LoopMeas{.t = t, .hazard = 0.5};
  });
  auto state_port = builder.tap<LoopState>("state");
  auto plan = builder.join<LoopMeas, LoopState, LoopPlan>(
      meas, state_port, [](const LoopMeas& m, const LoopState& s) {
        return LoopPlan{.t = m.t, .target = 10.0 - m.hazard, .v = s.v};
      });
  auto cmd = plan.map<LoopCmd>(
      [](const LoopPlan& p) { return LoopCmd{.t = p.t, .u = 1.5 * (p.target - p.v)}; });
  auto state = builder.from<LoopCmd, LoopState>("test.from.chassis", cmd, "state");
  ASSERT_TRUE(state.valid());
  state.sink([&](const LoopState&, const tianshu::core::Lineage& lin) {
    if (states.size() < 64) {
      states.push_back(lin.describe());
    }
  });

  tianshu::dsl::FlowRuntime runtime;
  runtime.run_for(builder.build(), std::chrono::milliseconds(400));

  ASSERT_GE(states.size(), 4U);
  // First state = init() report: rooted (no input in flight).
  EXPECT_EQ(states.front(), "lu/state#0");
  // Second state: derived from the first loop iteration — the sensor
  // root and the loop hops cross the component boundary.
  EXPECT_NE(states[1].find("lu/meas#"), std::string::npos);
  EXPECT_NE(states[1].find("-> lu/~1#"), std::string::npos);
  // Later states unroll further: each iteration adds a loop cycle.
  const auto& last = states.back();
  EXPECT_NE(last.find("lu/state#"), std::string::npos);
  EXPECT_LT(last.size(), 8000U);  // bounded by root-dedup + kMaxBranches
}
