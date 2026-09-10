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

// Compiler IR acceptance tests (ADR-0030 M-A): lowering, topological
// order, order-insensitive normalization hash, .dag/.conf exports.

#include "tianshu/compiler/ir.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "tianshu/core/component.h"
#include "tianshu/core/lineage.h"
#include "tianshu/core/message_traits.h"
#include "tianshu/sla/sla_analyzer.h"
// Link-time requirement: the wire factory templates live here.
// NOLINTNEXTLINE(misc-include-cleaner)
#include "tianshu/dsl/dsl_runtime.h"
#include "tianshu/dsl/flow.h"

// NOLINTNEXTLINE(misc-use-internal-linkage)  // traits must precede template use
struct TickMsg {
  std::uint64_t tick;
};
// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct DetectMsg {
  std::uint64_t tick;
};
// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct FuseMsg {
  std::uint64_t tick;
};

TIANSHU_TRAITS_POD(TickMsg, "ir.TickMsg");
TIANSHU_TRAITS_POD(DetectMsg, "ir.DetectMsg");
TIANSHU_TRAITS_POD(FuseMsg, "ir.FuseMsg");

namespace {
class IrDriver final : public tianshu::core::TimerSourceComponent<TickMsg> {
 public:
  explicit IrDriver(std::string name) : TimerSourceComponent(std::move(name)) {}

 protected:
  void proc() override { publish(TickMsg{.tick = tick_++}); }

  [[nodiscard]] std::string_view out_channel() const override { return {}; }

 private:
  std::uint64_t tick_{0};
};

class IrPass final : public tianshu::core::Component<TickMsg, DetectMsg> {
 public:
  explicit IrPass(std::string name) : Component(std::move(name)) {}

 protected:
  void proc(const TickMsg& in) override { publish(DetectMsg{.tick = in.tick}); }

  [[nodiscard]] std::string_view out_channel() const override { return {}; }
};

class IrFuse final : public tianshu::core::TwoInputComponent<TickMsg, DetectMsg, FuseMsg> {
 public:
  explicit IrFuse(std::string name) : TwoInputComponent(std::move(name)) {}

 protected:
  void proc(const TickMsg& in0, const DetectMsg& in1) override {
    publish(FuseMsg{.tick = in0.tick + in1.tick});
  }

  [[nodiscard]] std::string_view out_channel() const override { return {}; }
};
}  // namespace

TIANSHU_REGISTER_COMPONENT(IrDriver, "ir.from.driver")
TIANSHU_REGISTER_COMPONENT(IrPass, "ir.from.pass")
TIANSHU_REGISTER_COMPONENT(IrFuse, "ir.from.fuse")

namespace {

namespace compiler = tianshu::compiler;
namespace core = tianshu::core;
namespace dsl = tianshu::dsl;
namespace sla = tianshu::sla;

dsl::Flow make_chain(const std::string& name) {
  dsl::FlowBuilder b(name);
  auto mapped = b.source<TickMsg>("cam", std::chrono::milliseconds(10), [](std::uint64_t t) {
                   return TickMsg{.tick = t};
                 }).map<DetectMsg>([](const TickMsg& in) { return DetectMsg{.tick = in.tick}; });
  mapped.with_wcet(std::chrono::milliseconds(8));
  mapped.sink([](const DetectMsg&, const core::Lineage&) {});
  return b.build();
}

TEST(IrTest, LowersFlowWithKindsAndWcet) {
  const auto graph = compiler::IrGraph::from_flow(make_chain("ir_a"));

  ASSERT_EQ(graph.nodes().size(), 3U);  // source + map + sink
  EXPECT_EQ(graph.nodes()[0].kind, "source");
  EXPECT_EQ(graph.nodes()[1].kind, "map");
  EXPECT_EQ(graph.nodes()[2].kind, "sink");
  EXPECT_EQ(graph.nodes()[1].wcet, std::chrono::milliseconds(8));
  EXPECT_FALSE(graph.nodes()[1].output.empty());
}

TEST(IrTest, TopologicalOrderSortsProducersFirst) {
  const auto graph = compiler::IrGraph::from_flow(make_chain("ir_b"));
  EXPECT_LT(graph.nodes()[0].topo_order, graph.nodes()[1].topo_order);
  EXPECT_LT(graph.nodes()[1].topo_order, graph.nodes()[2].topo_order);
}

TEST(IrTest, HashIsStableAcrossRuns) {
  auto g1 = compiler::IrGraph::from_flow(make_chain("ir_c"));
  auto g2 = compiler::IrGraph::from_flow(make_chain("ir_c"));
  g1.normalize();
  g2.normalize();
  EXPECT_EQ(g1.stable_hash(), g2.stable_hash());
}

TEST(IrTest, HashIsInsensitiveToDeclarationOrder) {
  // Branch B declared before branch A in the second builder: anonymous
  // channel numbering differs pre-normalization, must converge after.
  dsl::FlowBuilder b1("ir_d");
  auto a1m = b1.source<TickMsg>("a", std::chrono::milliseconds(10), [](std::uint64_t t) {
                 return TickMsg{.tick = t};
               }).map<DetectMsg>([](const TickMsg& in) { return DetectMsg{.tick = in.tick}; });
  auto b1m = b1.source<TickMsg>("b", std::chrono::milliseconds(10), [](std::uint64_t t) {
                 return TickMsg{.tick = t};
               }).map<DetectMsg>([](const TickMsg& in) { return DetectMsg{.tick = in.tick}; });
  auto j1 = b1.join<DetectMsg, DetectMsg, FuseMsg>(
      a1m, b1m,
      [](const DetectMsg& x, const DetectMsg& y) { return FuseMsg{.tick = x.tick + y.tick}; });
  (void)j1;

  dsl::FlowBuilder b2("ir_d");
  auto b2m = b2.source<TickMsg>("b", std::chrono::milliseconds(10), [](std::uint64_t t) {
                 return TickMsg{.tick = t};
               }).map<DetectMsg>([](const TickMsg& in) { return DetectMsg{.tick = in.tick}; });
  auto a2m = b2.source<TickMsg>("a", std::chrono::milliseconds(10), [](std::uint64_t t) {
                 return TickMsg{.tick = t};
               }).map<DetectMsg>([](const TickMsg& in) { return DetectMsg{.tick = in.tick}; });
  auto j2 = b2.join<DetectMsg, DetectMsg, FuseMsg>(
      b2m, a2m,
      [](const DetectMsg& y, const DetectMsg& x) { return FuseMsg{.tick = x.tick + y.tick}; });
  (void)j2;

  auto g1 = compiler::IrGraph::from_flow(b1.build());
  auto g2 = compiler::IrGraph::from_flow(b2.build());
  g1.normalize();
  g2.normalize();
  EXPECT_EQ(g1.stable_hash(), g2.stable_hash());
}

TEST(IrTest, DagExportCarriesEveryChannel) {
  const auto graph = compiler::IrGraph::from_flow(make_chain("ir_e"));
  const std::string dag = graph.export_dag();

  EXPECT_NE(dag.find("kind = \"source\""), std::string::npos);
  EXPECT_NE(dag.find("kind = \"map\""), std::string::npos);
  EXPECT_NE(dag.find("kind = \"sink\""), std::string::npos);
  for (const auto& n : graph.nodes()) {
    if (!n.output.empty()) {
      EXPECT_NE(dag.find(n.output), std::string::npos);
    }
  }
  EXPECT_NE(dag.find("hash = \""), std::string::npos);
}

TEST(IrTest, ConfExportMatchesSlaReport) {
  dsl::FlowBuilder b("ir_f");
  b.source<TickMsg>("cam", std::chrono::milliseconds(10),
                    [](std::uint64_t t) { return TickMsg{.tick = t}; })
      .map<DetectMsg>([](const TickMsg& in) { return DetectMsg{.tick = in.tick}; })
      .with_wcet(std::chrono::milliseconds(8))
      .sink([](const DetectMsg&, const core::Lineage&) {})
      .with_sla(sla::Sla{.deadline = std::chrono::milliseconds(50)});
  const auto flow = b.build();

  const auto graph = compiler::IrGraph::from_flow(flow);
  const std::string conf = graph.export_conf();

  EXPECT_NE(conf.find("sla_ok = true"), std::string::npos);
  for (const auto& budget : flow.sla_report().budgets) {
    EXPECT_NE(conf.find("channel = \"" + budget.channel + "\""), std::string::npos);
    EXPECT_NE(conf.find("planned_us = " + std::to_string(budget.planned.count())),
              std::string::npos);
  }
}

TEST(IrTest, NormalizeKeepsNodeCountAndAnonShape) {
  auto graph = compiler::IrGraph::from_flow(make_chain("ir_g"));
  const std::size_t nodes_before = graph.nodes().size();
  std::size_t anon_before = 0;
  for (const auto& n : graph.nodes()) {
    if (n.output.find("/~") != std::string::npos) {
      ++anon_before;
    }
  }

  graph.normalize();

  EXPECT_EQ(graph.nodes().size(), nodes_before);
  std::size_t anon_after = 0;
  for (const auto& n : graph.nodes()) {
    if (n.output.find("/~") != std::string::npos) {
      ++anon_after;
    }
  }
  EXPECT_EQ(anon_before, anon_after);
}

}  // namespace

TEST(IrTest, LowersEveryNodeKindAndSaturationConf) {
  dsl::FlowBuilder b("ir_all");
  // source + map + wcet
  auto mapped = b.source<TickMsg>("t", std::chrono::milliseconds(10), [](std::uint64_t t) {
                   return TickMsg{.tick = t};
                 }).map<DetectMsg>([](const TickMsg& in) { return DetectMsg{.tick = in.tick}; });
  mapped.with_wcet(std::chrono::milliseconds(8));
  // op: box-style read-write node (handle/on_init impl struct per ADR-0024)
  struct BoxOp {
    static void on_init(dsl::OpPub<FuseMsg>& /*pub*/) {}
    static void handle(const DetectMsg& in, dsl::OpPub<FuseMsg>& pub) {
      pub.publish(FuseMsg{.tick = in.tick});
    }
  };
  b.op<DetectMsg, FuseMsg>(mapped, "boxed", BoxOp{});
  // stateful
  struct NoFold {
    static void on_init(dsl::OpPub<DetectMsg>& /*out*/, dsl::OpPub<TickMsg>& /*state*/) {}
    static void handle(const DetectMsg& in, dsl::OpPub<DetectMsg>& out,
                       dsl::OpPub<TickMsg>& /*state*/) {
      out.publish(DetectMsg{.tick = in.tick});
    }
  };
  b.stateful<DetectMsg, TickMsg>(mapped, "st_out", "st_acc", NoFold{});
  // span over trigger+data (both TickMsg chains)
  const auto trig = b.source<TickMsg>("g", std::chrono::milliseconds(20),
                                      [](std::uint64_t t) { return TickMsg{.tick = t}; });
  b.span_join<FuseMsg>(
      trig, trig,
      [](const TickMsg& trig_msg) {
        return std::pair<std::uint64_t, std::uint64_t>(trig_msg.tick, trig_msg.tick + 5);
      },
      [](const TickMsg& msg) { return msg.tick; },
      [](const TickMsg& trig_msg, const dsl::Slice<TickMsg>& slice) {
        static_cast<void>(trig_msg);
        return FuseMsg{.tick = slice.items.size()};
      });

  auto graph = compiler::IrGraph::from_flow(b.build());
  std::size_t sources = 0;
  std::size_t maps = 0;
  std::size_t ops = 0;
  std::size_t statefuls = 0;
  std::size_t spans = 0;
  for (const auto& n : graph.nodes()) {
    if (n.kind == "source") {
      ++sources;
    } else if (n.kind == "map") {
      ++maps;
    } else if (n.kind == "op") {
      ++ops;
    } else if (n.kind == "stateful") {
      ++statefuls;
    } else if (n.kind == "span") {
      ++spans;
    }
  }
  EXPECT_EQ(sources, 2U);  // "t" + "g"
  EXPECT_EQ(maps, 1U);
  EXPECT_EQ(ops, 1U);
  EXPECT_EQ(statefuls, 1U);
  EXPECT_EQ(spans, 1U);

  // Normalize keeps the node count and ordering is idempotent.
  graph.normalize();
  graph.normalize();
  EXPECT_EQ(graph.nodes().size(), 6U);
}

// from() references lower to IR: source-like (no inputs), one-input, and
// two-input (both channels in inputs) forms.
TEST(IrTest, LowersFromReferencesOfEveryArity) {
  dsl::FlowBuilder b("ir_froms");
  const auto ticks = b.from<TickMsg>("ir.from.driver", "drv", std::chrono::milliseconds(10));
  const auto dets = b.from<TickMsg, DetectMsg>("ir.from.pass", ticks, "det");
  b.from<FuseMsg>("ir.from.fuse", ticks, dets, "fused")
      .sink([](const FuseMsg&, const tianshu::core::Lineage&) {});
  const auto graph = compiler::IrGraph::from_flow(b.build());

  const compiler::IrNode* src = nullptr;
  const compiler::IrNode* pass = nullptr;
  const compiler::IrNode* fuse = nullptr;
  for (const auto& n : graph.nodes()) {
    if (n.output == "ir_froms/drv") {
      src = &n;
    } else if (n.output == "ir_froms/det") {
      pass = &n;
    } else if (n.output == "ir_froms/fused") {
      fuse = &n;
    }
  }
  ASSERT_NE(src, nullptr);
  ASSERT_NE(pass, nullptr);
  ASSERT_NE(fuse, nullptr);
  EXPECT_EQ(src->kind, "source");
  EXPECT_TRUE(src->inputs.empty());
  EXPECT_EQ(pass->kind, "from");
  EXPECT_EQ(pass->inputs, (std::vector<std::string>{"ir_froms/drv"}));
  EXPECT_EQ(fuse->kind, "from");
  EXPECT_EQ(fuse->inputs, (std::vector<std::string>{"ir_froms/drv", "ir_froms/det"}));
}
