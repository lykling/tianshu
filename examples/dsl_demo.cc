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

// DSL v0 demo: declarative flow with automatic lineage (ADR-0021/0022).
//
//   ticks (20 Hz) --~0--> doubled --~1--> scaled --> sink
//
// The sink prints per-message values and their full lineage chains.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

#include "tianshu/core/lineage.h"
#include "tianshu/core/message_traits.h"
#include "tianshu/dsl/dsl_runtime.h"
#include "tianshu/dsl/flow.h"
#include "tianshu/sla/sla_analyzer.h"

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

TIANSHU_TRAITS_POD(TickMsg, "demo.TickMsg");
TIANSHU_TRAITS_POD(DoubledMsg, "demo.DoubledMsg");
TIANSHU_TRAITS_POD(ScaledMsg, "demo.ScaledMsg");

int main() {
  using tianshu::core::Lineage;

  std::vector<std::string> samples;

  const auto flow = [&]() -> tianshu::dsl::Flow {
    try {
      return tianshu::dsl::FlowBuilder("demo")
          .source<TickMsg>("ticks", std::chrono::milliseconds(20),
                           [](std::uint64_t t) { return TickMsg{.tick = t}; })
          .map<DoubledMsg>([](const TickMsg& in) {
            return DoubledMsg{.tick = in.tick, .value = static_cast<double>(in.tick) * 2};
          })
          .with_wcet(std::chrono::microseconds(120))
          .map<ScaledMsg>([](const DoubledMsg& in) {
            return ScaledMsg{.tick = in.tick, .value = in.value + 0.5};
          })
          .with_wcet(std::chrono::microseconds(80))
          .sink([&](const ScaledMsg& msg, const Lineage& lin) {
            if (msg.tick < 3 || msg.tick % 10 == 0) {
              samples.push_back("tick=" + std::to_string(msg.tick) + " value=" +
                                std::to_string(msg.value) + "  lineage: " + lin.describe());
            }
          })
          // Load-time deadline verification + budget allocation (ADR-0029)
          .with_sla(tianshu::sla::Sla{.deadline = std::chrono::milliseconds(40)})
          .build();
    } catch (const std::exception& e) {
      static_cast<void>(std::fprintf(stderr, "flow rejected: %s\n", e.what()));
      std::exit(EXIT_FAILURE);
    }
  }();
  static_cast<void>(std::printf("flow: %s\n", flow.describe().c_str()));
  static_cast<void>(std::printf("%s", flow.sla_report().format().c_str()));

  tianshu::dsl::FlowRuntime runtime;
  runtime.run_for(flow, std::chrono::milliseconds(600));

  for (const auto& s : samples) {
    static_cast<void>(std::printf("%s\n", s.c_str()));
  }
  static_cast<void>(std::printf("dsl_demo: %zu sampled messages\n", samples.size()));
  return 0;
}
