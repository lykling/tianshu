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

// Tests for the slice-input / state-as-data milestones: lineage range
// hops, channel history capture, the stateful primitive and its
// recovery protocol, span slices (ADR-0026/0027).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "tianshu/core/lineage.h"
#include "tianshu/core/message_traits.h"
#include "tianshu/dsl/dsl_runtime.h"
#include "tianshu/dsl/flow.h"

// NOLINTNEXTLINE(misc-use-internal-linkage)  // traits must precede template use
struct EvMsg {
  std::uint64_t t;
  double v;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct AccMsg {
  std::uint64_t n;
  double sum;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct OutMsg {
  std::uint64_t n;
  double val;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // traits must precede template use
struct LidarMsg {
  std::uint64_t t;
  std::uint32_t points;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct ImuMsg {
  std::uint64_t t;
  double w;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct CompMsg {
  std::uint64_t n_imu;
  std::uint64_t points;
};

TIANSHU_TRAITS_POD(LidarMsg, "ss.LidarMsg");
TIANSHU_TRAITS_POD(ImuMsg, "ss.ImuMsg");
TIANSHU_TRAITS_POD(CompMsg, "ss.CompMsg");
TIANSHU_TRAITS_POD(EvMsg, "ss.EvMsg");
TIANSHU_TRAITS_POD(AccMsg, "ss.AccMsg");
TIANSHU_TRAITS_POD(OutMsg, "ss.OutMsg");

namespace {

TEST(LineageRangeTest, RangeRenderAndMerge) {
  const auto slice = tianshu::core::Lineage::rooted_range("imu", 102, 121);
  EXPECT_EQ(slice.describe(), "imu#102..#121");

  auto trig = tianshu::core::Lineage::rooted("lidar", 7);
  trig.merge(slice);
  trig.add_hop({.channel = "fus", .seq = 3});
  EXPECT_EQ(trig.describe(), "lidar#7 -> fus#3 | imu#102..#121 -> fus#3");

  const tianshu::core::LineageHop single{.channel = "x", .seq = 5};
  EXPECT_FALSE(single.is_range());
}

TEST(LineageRangeTest, AddRangeHopRendersChain) {
  auto lin = tianshu::core::Lineage::rooted("src", 1);
  lin.add_range_hop("imu", 8, 25);
  EXPECT_EQ(lin.describe(), "src#1 -> imu#8..#25");
}

TEST(ChannelHistoryTest, CapturesSeqBytesAndLineageBounded) {
  tianshu::dsl::FlowRuntime rt;
  const std::string ch = "ss/ev";
  for (std::uint64_t i = 0; i < 80; ++i) {
    const EvMsg msg{.t = i, .v = static_cast<double>(i)};
    rt.publish_bytes(ch, &msg, sizeof(msg), tianshu::core::Lineage::rooted(ch, i));
  }
  const auto* hist = rt.history(ch);
  ASSERT_NE(hist, nullptr);
  // Bounded depth (64): the oldest retained entry is seq 16.
  ASSERT_EQ(hist->entries().size(), 64U);
  EXPECT_EQ(hist->entries().front().seq, 16U);
  EXPECT_EQ(hist->entries().back().seq, 79U);
  // Bytes round-trip: POD memcpy back.
  const auto& e = hist->entries().back();
  ASSERT_EQ(e.bytes.size(), sizeof(EvMsg));
  EvMsg back{};
  std::memcpy(&back, e.bytes.data(), sizeof(back));
  EXPECT_EQ(back.t, 79U);
  EXPECT_DOUBLE_EQ(back.v, 79.0);
}

TEST(ChannelHistoryTest, AbsentChannelHasNoHistory) {
  const tianshu::dsl::FlowRuntime rt;
  EXPECT_EQ(rt.history("ss/never"), nullptr);
}

}  // namespace

// v1 stateful contract: on_init(out_pub, state_pub); handle(in, out_pub,
// state_pub). The fold demonstrates continuous state versioning (one
// state message per absorbed input, ADR-0027 continuous mode).
// NOLINTNEXTLINE(misc-use-internal-linkage)  // wire template needs linkage
class FoldOp {
 public:
  FoldOp() = default;
  FoldOp(std::uint64_t n0, double sum0) : n_(n0), sum_(sum0) {}

  static void on_init(tianshu::dsl::OpPub<OutMsg>& /*out*/, tianshu::dsl::OpPub<AccMsg>& /*st*/) {}

  void handle(const EvMsg& e, tianshu::dsl::OpPub<OutMsg>& out, tianshu::dsl::OpPub<AccMsg>& st) {
    sum_ += e.v;
    ++n_;
    st.publish(AccMsg{.n = n_, .sum = sum_});
    out.publish(OutMsg{.n = n_, .val = sum_});
  }

  [[nodiscard]] std::uint64_t n() const { return n_; }
  [[nodiscard]] double sum() const { return sum_; }

 private:
  std::uint64_t n_{0};
  double sum_{0.0};
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // wire template instantiation needs linkage
tianshu::dsl::Flow make_fold_flow(const std::string& name, FoldOp impl) {
  tianshu::dsl::FlowBuilder builder(name);
  const auto ev = builder.tap<EvMsg>("ev");
  const auto out = builder.stateful<OutMsg, AccMsg>(ev, "out", "acc", impl);
  static_cast<void>(out);
  return builder.build();
}

namespace {

std::vector<EvMsg> publish_events(tianshu::dsl::FlowRuntime& rt, const std::string& ch,
                                  std::uint64_t lo, std::uint64_t hi) {
  std::vector<EvMsg> events;
  for (std::uint64_t i = lo; i < hi; ++i) {
    const EvMsg e{.t = i, .v = 1.0 + static_cast<double>(i % 7)};
    events.push_back(e);
    rt.publish_bytes(ch, &e, sizeof(e), tianshu::core::Lineage::rooted(ch, i));
  }
  return events;
}

TEST(StatefulOpTest, EveryInputProducesStateVersionWithExactLineage) {
  tianshu::dsl::FlowRuntime rt;
  const auto flow = make_fold_flow("sv", FoldOp{});
  for (const auto& decl : flow.statefuls()) {
    decl.wire(rt);
  }
  const std::string ev_ch = "sv/ev";
  static_cast<void>(publish_events(rt, ev_ch, 0, 5));

  const auto* acc = rt.history("sv/acc");
  ASSERT_NE(acc, nullptr);
  ASSERT_EQ(acc->entries().size(), 5U);
  for (std::size_t k = 0; k < 5; ++k) {
    // State lineage = input root + state hop: provenance points at the
    // exact input that produced this version.
    EXPECT_EQ(acc->entries()[k].lineage.describe(),
              "sv/ev#" + std::to_string(k) + " -> sv/acc#" + std::to_string(k));
  }
  AccMsg last{};
  std::memcpy(&last, acc->entries().back().bytes.data(), sizeof(last));
  EXPECT_EQ(last.n, 5U);
}

TEST(StatefulOpTest, DescribeRendersStatefulEdge) {
  const auto flow = make_fold_flow("sv", FoldOp{});
  EXPECT_NE(flow.describe().find("stateful[sv/ev -> sv/out + sv/acc]"), std::string::npos);
}

TEST(StatefulRecoveryTest, RecoverFromStatePlusSuffixReplayMatchesFullRun) {
  // Reference run: absorb inputs 0..99.
  tianshu::dsl::FlowRuntime full_rt;
  auto full_flow = make_fold_flow("full", FoldOp{});
  for (const auto& decl : full_flow.statefuls()) {
    decl.wire(full_rt);
  }
  static_cast<void>(publish_events(full_rt, "full/ev", 0, 100));
  const auto* full_acc = full_rt.history("full/acc");
  ASSERT_NE(full_acc, nullptr);
  AccMsg full_final{};
  std::memcpy(&full_final, full_acc->entries().back().bytes.data(), sizeof(full_final));

  // Crashed run: the stream kept flowing (all inputs 0..99 are in the
  // history), but the operator is recovered from an EARLIER checkpoint —
  // state@59 — exactly what snapshot-mode recovery looks like.
  tianshu::dsl::FlowRuntime crash_rt;
  auto crash_flow = make_fold_flow("cr", FoldOp{});
  for (const auto& decl : crash_flow.statefuls()) {
    decl.wire(crash_rt);
  }
  static_cast<void>(publish_events(crash_rt, "cr/ev", 0, 100));
  const auto* acc_hist = crash_rt.history("cr/acc");
  const auto* ev_hist = crash_rt.history("cr/ev");
  ASSERT_NE(acc_hist, nullptr);
  ASSERT_NE(ev_hist, nullptr);
  ASSERT_EQ(acc_hist->entries().size(), 64U);  // bounded: 100 versions, 64 kept
  ASSERT_EQ(ev_hist->entries().size(), 64U);

  // Recovery protocol: pick the checkpoint at state seq 59 (the ring is
  // bounded, so locate by seq, not index); its lineage says which input
  // it absorbed (branch root seq), so replay only the suffix after it.
  const tianshu::dsl::detail::HistoryEntry* state_k = nullptr;
  for (const auto& e : acc_hist->entries()) {
    if (e.seq == 59U) {
      state_k = &e;
      break;
    }
  }
  ASSERT_NE(state_k, nullptr);
  AccMsg recovered{};
  std::memcpy(&recovered, state_k->bytes.data(), sizeof(recovered));
  ASSERT_EQ(recovered.n, 60U);
  const std::uint64_t absorbed = state_k->lineage.root().seq;
  ASSERT_EQ(absorbed, 59U);

  // Recovered runtime: op state = recovered version; replay the suffix.
  tianshu::dsl::FlowRuntime rec_rt;
  auto rec_flow = make_fold_flow("rec", FoldOp{recovered.n, recovered.sum});
  for (const auto& decl : rec_flow.statefuls()) {
    decl.wire(rec_rt);
  }
  for (const auto& entry : ev_hist->entries()) {
    if (entry.seq <= absorbed) {
      continue;
    }
    rec_rt.publish_bytes("rec/ev", entry.bytes.data(), entry.bytes.size(),
                         tianshu::core::Lineage::rooted("rec/ev", entry.seq));
  }

  const auto* rec_acc = rec_rt.history("rec/acc");
  ASSERT_NE(rec_acc, nullptr);
  ASSERT_EQ(rec_acc->entries().size(), 40U);
  AccMsg rec_final{};
  std::memcpy(&rec_final, rec_acc->entries().back().bytes.data(), sizeof(rec_final));
  // Exact match with the uninterrupted run: state-as-data + suffix
  // replay is lossless (ADR-0027 recovery protocol).
  EXPECT_EQ(rec_final.n, full_final.n);
  EXPECT_DOUBLE_EQ(rec_final.sum, full_final.sum);
  // And the first recovered version's lineage points at its input.
  EXPECT_EQ(rec_acc->entries().front().lineage.describe(), "rec/ev#60 -> rec/acc#0");
}

TEST(SpanJoinTest, MaterializesTriggerAlignedSliceWithRangeLineage) {
  tianshu::dsl::FlowBuilder builder("sp");
  auto lidar = builder.tap<LidarMsg>("lidar");
  auto imu = builder.tap<ImuMsg>("imu");
  auto comp = builder.span_join<CompMsg>(
      lidar, imu,
      [](const LidarMsg& trig) {
        return std::pair<std::uint64_t, std::uint64_t>((trig.t * 20) - 18, trig.t * 20);
      },
      [](const ImuMsg& m) { return m.t; },
      [](const LidarMsg& trig, const tianshu::dsl::Slice<ImuMsg>& slice) {
        return CompMsg{.n_imu = slice.items.size(), .points = trig.points};
      });
  std::vector<std::string> lineage_samples;
  std::vector<CompMsg> comps;
  comp.sink([&](const CompMsg& c, const tianshu::core::Lineage& lin) {
    comps.push_back(c);
    if (lineage_samples.size() < 8) {
      lineage_samples.push_back(lin.describe());
    }
  });
  const auto flow = builder.build();
  EXPECT_NE(flow.describe().find("span[sp/lidar x sp/imu -> sp/~"), std::string::npos);

  tianshu::dsl::FlowRuntime rt;
  for (const auto& decl : flow.spans()) {
    decl.wire(rt);
  }
  for (const auto& decl : flow.sinks()) {
    decl.wire(rt);
  }

  for (std::uint64_t i = 0; i < 41; ++i) {  // t=0..40: both spans [2,20] and [22,40] fully covered
    const ImuMsg m{.t = i, .w = 0.1 * static_cast<double>(i)};
    rt.publish_bytes("sp/imu", &m, sizeof(m), tianshu::core::Lineage::rooted("sp/imu", i));
  }
  const LidarMsg l1{.t = 1, .points = 1200};
  rt.publish_bytes("sp/lidar", &l1, sizeof(l1), tianshu::core::Lineage::rooted("sp/lidar", 0));
  const LidarMsg l2{.t = 2, .points = 1150};
  rt.publish_bytes("sp/lidar", &l2, sizeof(l2), tianshu::core::Lineage::rooted("sp/lidar", 1));

  ASSERT_EQ(comps.size(), 2U);
  EXPECT_EQ(comps[0].n_imu, 19U);
  EXPECT_EQ(comps[1].n_imu, 19U);
  EXPECT_EQ(lineage_samples[0], "sp/lidar#0 -> sp/~0#0 | sp/imu#2..#20 -> sp/~0#0");
  EXPECT_EQ(lineage_samples[1], "sp/lidar#1 -> sp/~0#1 | sp/imu#22..#40 -> sp/~0#1");
}

TEST(SpanJoinTest, EmptySliceOmitsRangeBranch) {
  tianshu::dsl::FlowBuilder builder("se");
  auto lidar = builder.tap<LidarMsg>("lidar");
  auto imu = builder.tap<ImuMsg>("imu");
  auto comp = builder.span_join<CompMsg>(
      lidar, imu,
      [](const LidarMsg& trig) {
        return std::pair<std::uint64_t, std::uint64_t>(trig.t + 100, trig.t + 200);
      },
      [](const ImuMsg& m) { return m.t; },
      [](const LidarMsg&, const tianshu::dsl::Slice<ImuMsg>& slice) {
        return CompMsg{.n_imu = slice.items.size(), .points = 0};
      });
  std::vector<std::string> lineage_samples;
  comp.sink([&](const CompMsg&, const tianshu::core::Lineage& lin) {
    if (lineage_samples.size() < 4) {
      lineage_samples.push_back(lin.describe());
    }
  });
  const auto flow = builder.build();
  tianshu::dsl::FlowRuntime rt;
  for (const auto& decl : flow.spans()) {
    decl.wire(rt);
  }
  for (const auto& decl : flow.sinks()) {
    decl.wire(rt);
  }
  const ImuMsg m{.t = 1, .w = 0.0};
  rt.publish_bytes("se/imu", &m, sizeof(m), tianshu::core::Lineage::rooted("se/imu", 0));
  const LidarMsg l{.t = 0, .points = 10};
  rt.publish_bytes("se/lidar", &l, sizeof(l), tianshu::core::Lineage::rooted("se/lidar", 0));

  ASSERT_EQ(lineage_samples.size(), 1U);
  EXPECT_EQ(lineage_samples[0], "se/lidar#0 -> se/~0#0");
}

}  // namespace
