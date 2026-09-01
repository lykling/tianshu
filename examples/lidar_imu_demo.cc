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

// Slice-input demo (ADR-0026): lidar x IMU motion compensation — the
// canonical trigger-aligned span. One 10Hz lidar frame needs ALL the
// 200Hz IMU samples inside its sweep interval; AllLatest cannot express
// this, span_join materializes the slice and the output lineage carries
// the exact member range.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "tianshu/core/lineage.h"
#include "tianshu/core/message_traits.h"
#include "tianshu/dsl/dsl_runtime.h"
#include "tianshu/dsl/flow.h"

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

TIANSHU_TRAITS_POD(LidarMsg, "li.LidarMsg");
TIANSHU_TRAITS_POD(ImuMsg, "li.ImuMsg");
TIANSHU_TRAITS_POD(CompMsg, "li.CompMsg");

int main() {
  static_cast<void>(
      std::printf("== slice input: lidar x IMU motion compensation (ADR-0026) ==\n\n"));

  std::vector<std::string> samples;
  std::vector<std::uint64_t> imu_counts;

  tianshu::dsl::FlowBuilder builder("mm");
  auto lidar =
      builder.source<LidarMsg>("lidar", std::chrono::milliseconds(100), [](std::uint64_t t) {
        return LidarMsg{.t = t + 1, .points = 1200 + static_cast<std::uint32_t>((t % 3) * 50)};
      });
  auto imu = builder.source<ImuMsg>("imu", std::chrono::milliseconds(5), [](std::uint64_t t) {
    return ImuMsg{.t = t + 1, .w = 0.01 * static_cast<double>(t % 10)};
  });

  auto comp = builder.span_join<CompMsg>(
      lidar, imu,
      [](const LidarMsg& trig) {
        // Sweep interval: the last 18 IMU ticks before (and including)
        // this lidar frame's time — motion compensation input.
        return std::pair<std::uint64_t, std::uint64_t>((trig.t * 20) - 18, trig.t * 20);
      },
      [](const ImuMsg& m) { return m.t; },
      [](const LidarMsg& trig, const tianshu::dsl::Slice<ImuMsg>& slice) {
        double dw = 0.0;
        for (const auto& m : slice.items) {
          dw += m.w;
        }
        static_cast<void>(dw);
        return CompMsg{.n_imu = slice.items.size(), .points = trig.points};
      });
  comp.sink([&](const CompMsg& c, const tianshu::core::Lineage& lin) {
    imu_counts.push_back(c.n_imu);
    if (samples.size() < 6) {
      samples.push_back("points=" + std::to_string(c.points) + "  imu_samples=" +
                        std::to_string(c.n_imu) + "\n     lineage: " + lin.describe());
    }
  });

  const auto flow = builder.build();
  static_cast<void>(std::printf("flow: %s\n\n", flow.describe().c_str()));

  tianshu::dsl::FlowRuntime runtime;
  runtime.run_for(flow, std::chrono::milliseconds(1000));

  static_cast<void>(std::printf("compensated frames:\n"));
  for (const auto& s : samples) {
    static_cast<void>(std::printf("  %s\n", s.c_str()));
  }
  std::uint64_t total = 0;
  for (const auto n : imu_counts) {
    total += n;
  }
  if (!imu_counts.empty()) {
    static_cast<void>(std::printf(
        "\n%zu frames, avg %.1f IMU samples per frame "
        "(span = 19 ticks: one lidar sweep at 10Hz x IMU at 200Hz)\n",
        imu_counts.size(), static_cast<double>(total) / static_cast<double>(imu_counts.size())));
  }
  return 0;
}
