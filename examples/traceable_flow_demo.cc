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

// Traceable-flow demo (ADR-0030 M-D): the flow declares itself via
// REGISTER_TRACEABLE_FLOW; the launcher discovers it BY NAME, builds
// (the build is the dry-run trace: SLA runs, the graph materializes),
// compiles it to a .so artifact, and runs the compiled wiring. The
// README's target-API first line is now real code.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>

#include "tianshu/compiler/ir.h"
#include "tianshu/compiler/pipeline.h"
#include "tianshu/core/lineage.h"
#include "tianshu/core/message_traits.h"
#include "tianshu/dsl/dsl_runtime.h"
#include "tianshu/dsl/flow.h"
#include "tianshu/sla/sla_analyzer.h"

// NOLINTNEXTLINE(misc-use-internal-linkage)  // traits must precede template use
struct DemoTick {
  std::uint64_t tick{0};
};
// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct DemoDoubled {
  std::uint64_t tick{0};
  double value{0.0};
};

TIANSHU_TRAITS_POD(DemoTick, "trace.DemoTick");
TIANSHU_TRAITS_POD(DemoDoubled, "trace.DemoDoubled");

namespace {

[[maybe_unused]] void declare_demo_flow(tianshu::dsl::FlowBuilder& b) {
  b.source<DemoTick>("ticks", std::chrono::milliseconds(5),
                     [](std::uint64_t t) { return DemoTick{.tick = t}; })
      .map<DemoDoubled>([](const DemoTick& in) {
        return DemoDoubled{.tick = in.tick, .value = static_cast<double>(in.tick) * 2};
      })
      .with_wcet(std::chrono::microseconds(80))
      .sink([](const DemoDoubled& msg, const tianshu::core::Lineage& lin) {
        if (msg.tick < 3) {
          static_cast<void>(std::printf("[sink] tick=%llu value=%.1f  %s\n",
                                        static_cast<unsigned long long>(msg.tick), msg.value,
                                        lin.describe().c_str()));
        }
      })
      .with_sla(tianshu::sla::Sla{.deadline = std::chrono::milliseconds(20)})
      .with_fallback("demo_traceable_lite");
}

[[maybe_unused]] void declare_demo_flow_lite(tianshu::dsl::FlowBuilder& b) {
  b.source<DemoTick>("lite_ticks", std::chrono::milliseconds(50), [](std::uint64_t t) {
     return DemoTick{.tick = t};
   }).sink([](const DemoTick&, const tianshu::core::Lineage&) {});
}

}  // namespace

REGISTER_TRACEABLE_FLOW("demo_traceable", declare_demo_flow)
REGISTER_TRACEABLE_FLOW("demo_traceable_lite", declare_demo_flow_lite)

int main() try {
  static_cast<void>(std::printf("== registered flows ==\n"));
  for (const auto& name : tianshu::dsl::registered_flow_names()) {
    static_cast<void>(std::printf("  %s\n", name.c_str()));
  }

  // Dry-run by name: build = trace (SLA verdict + graph).
  const auto flow = tianshu::dsl::build_registered_flow("demo_traceable");
  static_cast<void>(std::printf("\n== dry-run trace ==\n%s\n", flow.describe().c_str()));
  static_cast<void>(std::printf("%s", flow.sla_report().format().c_str()));

  auto graph = tianshu::compiler::IrGraph::from_flow(flow);
  graph.normalize();
  static_cast<void>(std::printf("artifact hash: %s\n", graph.stable_hash().c_str()));
  static_cast<void>(std::printf("fallback ladder: %s\n", flow.fallback_flow().empty()
                                                             ? "(none)"
                                                             : flow.fallback_flow().c_str()));

  // Compile and run the artifact.
  auto compiled = tianshu::compiler::Pipeline::compile(flow);
  static_cast<void>(std::printf("\n== compiled run (%s, %s) ==\n",
                                compiled.valid() ? "artifact" : "degraded",
                                compiled.from_cache() ? "cached" : "fresh"));
  tianshu::dsl::FlowRuntime runtime;
  compiled.run(runtime, flow, std::chrono::milliseconds(60));
  const auto fallback = runtime.fallback_state();
  static_cast<void>(
      std::printf("degradation events: %llu\n", static_cast<unsigned long long>(fallback.events)));
  return 0;
} catch (const std::exception& e) {
  static_cast<void>(std::fprintf(stderr, "error: %s\n", e.what()));
  return 1;
}
