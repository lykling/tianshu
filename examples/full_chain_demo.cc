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

// Full-chain demo: sensor fusion -> perception -> prediction -> planning
// -> control -> chassis, with the chassis state FEED BACK into planning
// (a closed loop; DSL v0.5 join + multi-consumer, box primitive per
// ADR-0024).
//
//   radar/front (20ms) ---+
//   radar/rear  (25ms) ---join-> fused obstacles ---+
//   gnss       (100ms) ----------------------------join-> perception
//                                                        |
//                                                        v
//                                             map -> prediction ---+
//   chassis port (tap, cycle-breaker) ----------------------join-> planning
//                                                                      |
//                                                     map -> control --|
//                                                BOX "chassis" <-'  (from: init() publishes
//                                                                             the power-on state;
//                                                                             handle integrates
//                                                                             commands)
//                             chassis channel feeds planning AND printing (multi-consumer)
//
// The loop is real: planning sees the plant's speed, control computes the
// accel, the plant integrates it — watch the speed converge to target.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "tianshu/core/lineage.h"
#include "tianshu/core/message_traits.h"
#include "tianshu/dsl/dsl_runtime.h"
#include "tianshu/dsl/flow.h"

// NOLINTNEXTLINE(misc-use-internal-linkage)  // traits must precede template use
struct RadarMsg {
  std::uint64_t t;
  std::uint32_t radar_id;
  std::uint32_t n_targets;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct ObstacleList {
  std::uint64_t t;
  std::uint32_t n_obstacles;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct GnssMsg {
  std::uint64_t t;
  double x;
  double y;
  double heading;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct PerceptionOut {
  std::uint64_t t;
  std::uint32_t n_obstacles;
  double pose_x;
  double pose_y;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct PredictionOut {
  std::uint64_t t;
  std::uint32_t n_obstacles;
  double risk;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct ChassisState {
  std::uint64_t t;
  double speed;
  double steer;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct Plan {
  std::uint64_t t;
  double target_speed;
  double current_speed;
  double curvature;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct ControlCmd {
  std::uint64_t t;
  double accel;
  double steer_cmd;
};

TIANSHU_TRAITS_POD(RadarMsg, "demo.RadarMsg");
TIANSHU_TRAITS_POD(ObstacleList, "demo.ObstacleList");
TIANSHU_TRAITS_POD(GnssMsg, "demo.GnssMsg");
TIANSHU_TRAITS_POD(PerceptionOut, "demo.PerceptionOut");
TIANSHU_TRAITS_POD(PredictionOut, "demo.PredictionOut");
TIANSHU_TRAITS_POD(ChassisState, "demo.ChassisState");
TIANSHU_TRAITS_POD(Plan, "demo.Plan");
TIANSHU_TRAITS_POD(ControlCmd, "demo.ControlCmd");

namespace {

using tianshu::core::Lineage;

void print_sample(char tag, std::uint64_t t, const std::string& what, const Lineage& lin) {
  static_cast<void>(std::printf(  // NOLINT(concurrency-mt-unsafe)
      "[%c] t=%-4llu %s\n     lineage: %s\n", tag, static_cast<unsigned long long>(t), what.c_str(),
      lin.describe().c_str()));
}

}  // namespace

namespace {

struct ChassisLog {
  std::uint64_t t;
  double speed;
  std::size_t branches;
  std::string lineage;
};

tianshu::dsl::Flow build_avp_flow(std::vector<ChassisLog>& chassis_log) {
  tianshu::dsl::FlowBuilder builder("avp");

  // Registered drivers (ADR-0025): the flow names them + channels +
  // pacing — implementations live in the device library (linked in).
  auto radar_front =
      builder.from<RadarMsg>("avp.radar.front", "radar/front", std::chrono::milliseconds(20));
  auto radar_rear =
      builder.from<RadarMsg>("avp.radar.rear", "radar/rear", std::chrono::milliseconds(25));
  auto gnss = builder.from<GnssMsg>("avp.gnss", "gnss", std::chrono::milliseconds(100));

  auto fused = builder.join<RadarMsg, RadarMsg, ObstacleList>(
      radar_front, radar_rear, [](const RadarMsg& f, const RadarMsg& r) {
        return ObstacleList{.t = f.t, .n_obstacles = f.n_targets + r.n_targets};
      });

  auto perception = builder.join<ObstacleList, GnssMsg, PerceptionOut>(
      fused, gnss, [](const ObstacleList& obs, const GnssMsg& g) {
        return PerceptionOut{
            .t = obs.t, .n_obstacles = obs.n_obstacles, .pose_x = g.x, .pose_y = g.y};
      });

  auto prediction = perception.map<PredictionOut>([](const PerceptionOut& p) {
    const double risk = static_cast<double>(p.n_obstacles) * 0.08;
    return PredictionOut{.t = p.t, .n_obstacles = p.n_obstacles, .risk = risk};
  });

  // Cycle-breaker port: planning references the chassis channel before
  // the box writing it is constructed (feedback edge, ADR-0024).
  auto chassis_port = builder.tap<ChassisState>("chassis");

  auto plan = builder.join<PredictionOut, ChassisState, Plan>(
      prediction, chassis_port, [](const PredictionOut& pred, const ChassisState& ch) {
        const double target = 20.0 - (pred.risk * 8.0);
        const double curvature = 0.01 * static_cast<double>(pred.n_obstacles);
        return Plan{
            .t = pred.t, .target_speed = target, .current_speed = ch.speed, .curvature = curvature};
      });

  // Control ALSO closes on the chassis state: the join pairs each plan
  // with the latest MEASURED speed/steer — feedback control on the
  // actual state, not the plan's embedded copy (second consumer of the
  // same chassis channel).
  auto control = builder.join<Plan, ChassisState, ControlCmd>(
      plan, chassis_port, [](const Plan& p, const ChassisState& ch) {
        const double accel = 2.0 * (p.target_speed - ch.speed);
        const double steer_cmd = p.curvature + (0.2 * (p.curvature - ch.steer));
        return ControlCmd{.t = p.t, .accel = accel, .steer_cmd = steer_cmd};
      });

  control.sink([](const ControlCmd& c, const Lineage& lin) {
    if (c.t % 5 == 0) {
      char what[96];
      static_cast<void>(std::snprintf(  // NOLINT(concurrency-mt-unsafe)
          what, sizeof(what), "accel=%+.2f m/s2  steer_cmd=%+.3f", c.accel, c.steer_cmd));
      print_sample('C', c.t, what, lin);
    }
  });

  // The chassis as a REGISTERED component (ADR-0025): its init()
  // publishes the power-on report through the bridge — the feedback loop
  // ignites without any seed source.
  auto chassis = builder.from<ControlCmd, ChassisState>("avp.chassis", control, "chassis");

  perception.sink([](const PerceptionOut& p, const Lineage& lin) {
    if (p.t % 5 == 0) {
      print_sample('P', p.t,
                   "obstacles=" + std::to_string(p.n_obstacles) + "  pose=(" +
                       std::to_string(static_cast<int>(p.pose_x)) + "," +
                       std::to_string(static_cast<int>(p.pose_y)) + ")",
                   lin);
    }
  });

  plan.sink([](const Plan& p, const Lineage& lin) {
    if (p.t % 5 == 0) {
      print_sample('L', p.t,
                   "target=" + std::to_string(static_cast<int>(p.target_speed)) +
                       "m/s  chassis_v=" + std::to_string(static_cast<int>(p.current_speed)) +
                       "m/s",
                   lin);
    }
  });

  // Chassis channel consumers: planning's join above AND this sink
  // (multi-consumer) — the feedback edge and observability coexist.
  chassis.sink([&chassis_log](const ChassisState& c, const Lineage& lin) {
    if (chassis_log.size() < 200) {
      chassis_log.push_back({.t = c.t,
                             .speed = c.speed,
                             .branches = lin.branches().size(),
                             .lineage = lin.describe()});
    }
  });

  return builder.build();
}

}  // namespace

int main() try {
  std::vector<ChassisLog> chassis_log;

  const auto flow = build_avp_flow(chassis_log);
  static_cast<void>(std::printf("flow: %s\n\n", flow.describe().c_str()));

  tianshu::dsl::FlowRuntime runtime;
  runtime.run_for(flow, std::chrono::milliseconds(2000));

  static_cast<void>(std::printf("\nchassis trace (feedback loop):\n"));
  for (std::size_t i = 0; i < chassis_log.size(); i += 3) {
    const auto& c = chassis_log[i];
    static_cast<void>(std::printf(  // NOLINT(concurrency-mt-unsafe)
        "  t=%-4llu v=%6.2f m/s  branches=%zu\n", static_cast<unsigned long long>(c.t), c.speed,
        c.branches));
  }
  if (!chassis_log.empty()) {
    static_cast<void>(std::printf(  // NOLINT(concurrency-mt-unsafe)
        "\nfinal: v=%.2f m/s, last lineage (%zu branches):\n  %s\n", chassis_log.back().speed,
        chassis_log.back().branches, chassis_log.back().lineage.c_str()));
  }
  return 0;
} catch (const std::exception& e) {
  static_cast<void>(std::fprintf(stderr, "error: %s\n", e.what()));
  return 1;
}
