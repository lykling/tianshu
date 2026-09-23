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

// Dual-system end-to-end autopilot demo on a declarative TIANSHU flow.
//
//   System 1 (10Hz, one-stage net):  camera ---join-> backbone (BEV tokens,
//                                    biased depth) --join-> planner head
//   System 2 (1Hz, semantic layer):  adv tick --join-> advisory (the VLA
//                                    slot: directive from REDUNDANT context;
//                                    a real VLA replaces this join operator
//                                    and the flow declaration stays unchanged)
//   chassis_state -tap- feeds both joins above -------------------^
//
//   planner head -> rule safety shell (brake budget on redundant distance)
//                -> control (P + accel clamp) -> plant ("e2e.plant",
//                   registered box) -> chassis_state  (feedback loop)
//
// The story the demo tells (all numbers closed-form, see constants below):
//   - The mock net has an optimistic depth bias (+25m): alone it would
//     cruise into the obstacle at ~12 m/s.
//   - System 2 sees the REDUNDANT distance; inside the 40m semantic
//     horizon it flips to YIELD and caps the target at 6 m/s — the net
//     WITH advisory still arrives at ~6 m/s (its bias persists).
//   - The rule shell checks an explicit brake budget (v^2/2a + reaction
//     + margin) on the redundant distance, fires near d~7m, and the car
//     stops ~3.8m before the obstacle; a standstill guard holds the stop
//     (the still-biased net would otherwise re-accelerate).
//   - Every chassis message carries lineage back through the whole stack
//     to the exact camera frame (and advisory tick) that produced it.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tianshu/core/component.h"
#include "tianshu/core/lineage.h"
#include "tianshu/core/message_traits.h"
#include "tianshu/dsl/dsl_runtime.h"
#include "tianshu/dsl/flow.h"
#include "tianshu/sla/sla_analyzer.h"

// NOLINTNEXTLINE(misc-use-internal-linkage)  // traits must precede template use
struct CameraFrame {
  std::uint64_t t;
  double obstacle_x;  // world-frame detection (sensor row carries no ego state)
  std::uint32_t tokens;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct BevTokens {
  std::uint64_t t;
  double d_est;   // the NET's depth estimate (biased)
  double d_real;  // redundant channel the shell and System 2 trust
  double ego_v;
  std::uint32_t n_tokens;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct AdvisoryTick {
  std::uint64_t t;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct AdvisoryMsg {
  std::uint64_t t;
  double cap;  // speed cap the planner head must respect [m/s]
  bool yield;  // true = semantic layer demands yielding
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct PlanCmd {
  std::uint64_t t;
  double v_net;     // net's own profile target (before the advisory cap)
  double v_target;  // min(net profile, advisory cap)
  double d_real;
  double ego_v;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct DriveCmd {
  std::uint64_t t;
  double v_target;
  double ego_v;
  bool shell;  // true = rule shell overrode planner + advisory
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct AccelCmd {
  std::uint64_t t;
  double accel;  // already clamped to plant limits
  bool shell;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct ChassisState {
  std::uint64_t t;
  double x;
  double v;
};

TIANSHU_TRAITS_POD(CameraFrame, "e2e.CameraFrame");
TIANSHU_TRAITS_POD(BevTokens, "e2e.BevTokens");
TIANSHU_TRAITS_POD(AdvisoryTick, "e2e.AdvisoryTick");
TIANSHU_TRAITS_POD(AdvisoryMsg, "e2e.AdvisoryMsg");
TIANSHU_TRAITS_POD(PlanCmd, "e2e.PlanCmd");
TIANSHU_TRAITS_POD(DriveCmd, "e2e.DriveCmd");
TIANSHU_TRAITS_POD(AccelCmd, "e2e.AccelCmd");
TIANSHU_TRAITS_POD(ChassisState, "e2e.ChassisState");

namespace {

// World and policy constants (all SI; the scenario timeline is closed-form).
constexpr double kDt = 0.1;           // loop period (one camera frame) [s]
constexpr double kObstacleX = 70.0;   // fixed obstacle, world frame [m]
constexpr double kCruise = 12.0;      // cruise speed [m/s]
constexpr double kAccelMax = 4.0;     // plant accel clamp [m/s^2]
constexpr double kBrakeMax = 6.0;     // plant decel clamp [m/s^2]
constexpr double kComfDecel = 2.5;    // net's implied comfortable decel [m/s^2]
constexpr double kDepthBias = 25.0;   // net's optimistic depth error [m]
constexpr double kAdvHorizon = 40.0;  // System 2 semantic horizon [m]
constexpr double kYieldCap = 6.0;     // advisory speed cap while yielding [m/s]
constexpr double kReact = 0.3;        // shell reaction latency [s]
constexpr double kMargin = 2.0;       // shell standstill margin [m]
constexpr double kNetStopGap = 5.0;   // net's intended stop distance [m]
constexpr double kPGain = 2.5;        // longitudinal P gain [1/s]

// The plant as one registered component (ADR-0025): init() publishes the
// power-on report so the feedback loop bootstraps; proc() integrates the
// clamped accel into bicycle-lite kinematics.
class PlantComponent final : public tianshu::core::Component<AccelCmd, ChassisState> {
 public:
  explicit PlantComponent(std::string name) : Component(std::move(name)) {}

  bool init() override {
    publish(ChassisState{.t = 0, .x = 0.0, .v = 0.0});
    return true;
  }

 protected:
  void proc(const AccelCmd& cmd) override {
    v_ = std::max(0.0, v_ + (cmd.accel * kDt));
    x_ += v_ * kDt;
    publish(ChassisState{.t = cmd.t, .x = x_, .v = v_});
  }

  [[nodiscard]] std::string_view out_channel() const override { return {}; }

 private:
  double x_{0.0};
  double v_{0.0};
};

struct TickLog {
  std::uint64_t t;
  double x;
  double v;
  std::size_t branches;
  std::string lineage;
};

// Everything the sinks observe while the flow runs.
struct DemoStats {
  std::vector<TickLog> trace;
  std::uint64_t shell_ticks = 0;
  std::uint64_t shell_first_tick = 0;
  double shell_first_d = 0.0;
};

tianshu::dsl::Flow build_e2e_flow(DemoStats& stats) {
  using tianshu::core::Lineage;

  tianshu::dsl::FlowBuilder builder("e2e_ad");

  // System 1 sensor: 10Hz camera rows. The obstacle sits at a fixed world
  // position; ego-relative geometry is fused downstream.
  auto camera = builder.source<CameraFrame>(
      "camera", std::chrono::milliseconds(100),
      [](std::uint64_t t) { return CameraFrame{.t = t, .obstacle_x = kObstacleX, .tokens = 64}; });

  // Cycle-breaker port: both the backbone and System 2 need the measured
  // ego state written by the plant further down the chain (ADR-0024).
  auto ego = builder.tap<ChassisState>("chassis_state");

  // System 2 (the VLA slot): a semantic layer over the REDUNDANT
  // distance — inside the 40m horizon it flips to YIELD and caps the
  // speed. A real VLA (Phase 2: GPU/C ABI per ADR-0006/0007) replaces
  // this join's operator; the flow declaration does not change.
  //
  // DSL v0 AllLatest joins are synchronizers, not latches: every firing
  // consumes one message per input, so a genuinely slow (e.g. 1Hz)
  // directive would rate-limit the entire downstream loop to 1Hz. The
  // slow-system story therefore rides on WCET budget asymmetry rather
  // than on a slower publish rate.
  auto adv_tick =
      builder.source<AdvisoryTick>("adv_tick", std::chrono::milliseconds(100),
                                   [](std::uint64_t t) { return AdvisoryTick{.t = t}; });
  auto advisory = builder.join<AdvisoryTick, ChassisState, AdvisoryMsg>(
      adv_tick, ego, [](const AdvisoryTick&, const ChassisState& s) {
        const bool yield = (kObstacleX - s.x) <= kAdvHorizon;
        return AdvisoryMsg{.t = 0, .cap = yield ? kYieldCap : kCruise, .yield = yield};
      });

  // Backbone: camera x ego-state -> fused BEV tokens. This IS the
  // one-stage interface: the planner head consumes tokens, never a
  // structured obstacle list. The +25m bias models a mediocre net.
  auto backbone = builder.join<CameraFrame, ChassisState, BevTokens>(
      camera, ego, [](const CameraFrame& f, const ChassisState& s) {
        const double d_real = f.obstacle_x - s.x;
        return BevTokens{.t = f.t,
                         .d_est = d_real + kDepthBias,
                         .d_real = d_real,
                         .ego_v = s.v,
                         .n_tokens = f.tokens};
      });

  // Planner head: net profile from the BIASED depth (comfortable decel —
  // fine if d_est were true), capped by the System 2 directive.
  auto plan = builder.join<BevTokens, AdvisoryMsg, PlanCmd>(
      backbone, advisory, [](const BevTokens& b, const AdvisoryMsg& a) {
        const double v_profile =
            std::min(kCruise, std::sqrt(std::max(0.0, 2.0 * kComfDecel * (b.d_est - kNetStopGap))));
        return PlanCmd{.t = b.t,
                       .v_net = v_profile,
                       .v_target = std::min(v_profile, a.cap),
                       .d_real = b.d_real,
                       .ego_v = b.ego_v};
      });

  // Rule safety shell: check the REDUNDANT distance against an explicit
  // brake budget; the standstill guard holds the stop inside 7m.
  auto cmd = plan.map<DriveCmd>([](const PlanCmd& p) {
                   const double budget =
                       ((p.ego_v * p.ego_v) / (2.0 * kBrakeMax)) + (p.ego_v * kReact) + kMargin;
                   const bool engaged =
                       (p.d_real <= budget) || (p.d_real <= (kMargin + kNetStopGap));
                   return DriveCmd{.t = p.t,
                                   .v_target = engaged ? 0.0 : p.v_target,
                                   .ego_v = p.ego_v,
                                   .shell = engaged};
                 })
                 .with_wcet(std::chrono::microseconds(200));

  // Control: longitudinal P, saturated to the plant's accel/brake limits.
  auto control = cmd.map<AccelCmd>([](const DriveCmd& d) {
                      const double accel =
                          std::clamp(kPGain * (d.v_target - d.ego_v), -kBrakeMax, kAccelMax);
                      return AccelCmd{.t = d.t, .accel = accel, .shell = d.shell};
                    })
                     .with_wcet(std::chrono::microseconds(100));

  // The plant: a registered box referenced by name via from() — its
  // init() publishes the power-on state, igniting the feedback loop.
  auto plant = builder.from<AccelCmd, ChassisState>("e2e.plant", control, "chassis_state");

  // Observability: shell engagement bookkeeping on the command channel,
  // under the camera->command deadline.
  cmd.sink([&stats](const DriveCmd& d, const Lineage&) {
       if (d.shell) {
         ++stats.shell_ticks;
         if (stats.shell_ticks == 1) {
           stats.shell_first_tick = d.t;
         }
       }
     })
      .with_sla(tianshu::sla::Sla{.deadline = std::chrono::milliseconds(80)});

  // Chassis trace with per-message lineage (multi-consumer: the plant
  // output feeds the feedback joins above AND this sink), under the
  // end-to-end camera->chassis deadline (load-time verified, ADR-0029).
  plant
      .sink([&stats](const ChassisState& s, const Lineage& lin) {
        stats.trace.push_back({.t = s.t,
                               .x = s.x,
                               .v = s.v,
                               .branches = lin.branches().size(),
                               .lineage = lin.describe()});
      })
      .with_sla(tianshu::sla::Sla{.deadline = std::chrono::milliseconds(100)});

  return builder.build();
}

}  // namespace

TIANSHU_REGISTER_COMPONENT(PlantComponent, "e2e.plant")

int main() try {
  static_cast<void>(
      std::printf("== dual-system E2E autopilot: net + VLA-slot advisory + rule shell ==\n\n"));

  DemoStats stats;
  const auto flow = build_e2e_flow(stats);
  static_cast<void>(std::printf("flow: %s\n", flow.describe().c_str()));
  static_cast<void>(std::printf("%s", flow.sla_report().format().c_str()));

  tianshu::dsl::FlowRuntime runtime;
  runtime.run_for(flow, std::chrono::milliseconds(11000));

  for (const auto& stat : runtime.sla_snapshot()) {
    static_cast<void>(std::printf(  // NOLINT(concurrency-mt-unsafe)
        "[sla][runtime] %-12s planned=%lldus p50=%lldns p99=%lldns miss=%llu/%llu\n",
        stat.endpoint.c_str(), static_cast<long long>(stat.deadline.count()),
        static_cast<long long>(stat.p50.count()), static_cast<long long>(stat.p99.count()),
        static_cast<unsigned long long>(stat.miss_count),
        static_cast<unsigned long long>(stat.count)));
  }

  if (stats.trace.empty()) {
    static_cast<void>(std::fprintf(stderr, "error: no chassis messages produced\n"));
    return 1;
  }

  const TickLog& last = stats.trace.back();
  const double stop_distance = kObstacleX - last.x;

  // System 2's first yield, from the trace (first frame inside the horizon).
  const TickLog* yield_frame = nullptr;
  for (const auto& e : stats.trace) {
    if ((kObstacleX - e.x) <= kAdvHorizon) {
      yield_frame = &e;
      break;
    }
  }
  // Shell's first engagement distance from the trace.
  double shell_first_d = 0.0;
  for (const auto& e : stats.trace) {
    if (e.t == stats.shell_first_tick) {
      shell_first_d = kObstacleX - e.x;
      break;
    }
  }

  static_cast<void>(std::printf("\nchassis trace (every 5th frame):\n"));
  for (std::size_t i = 0; i < stats.trace.size(); i += 5) {
    const auto& e = stats.trace[i];
    static_cast<void>(std::printf(  // NOLINT(concurrency-mt-unsafe)
        "  t=%-4llu x=%6.2f m  v=%5.2f m/s\n", static_cast<unsigned long long>(e.t), e.x, e.v));
  }

  if (yield_frame != nullptr) {
    static_cast<void>(std::printf(  // NOLINT(concurrency-mt-unsafe)
        "\nsystem 2 (advisory): YIELD near t=%llu, d=%.1f m -> cap %.0f m/s\n",
        static_cast<unsigned long long>(yield_frame->t), kObstacleX - yield_frame->x, kYieldCap));
  }
  if (stats.shell_ticks > 0) {
    static_cast<void>(std::printf(  // NOLINT(concurrency-mt-unsafe)
        "safety shell: %llu frame(s) engaged, first at t=%llu, d=%.1f m\n",
        static_cast<unsigned long long>(stats.shell_ticks),
        static_cast<unsigned long long>(stats.shell_first_tick), shell_first_d));
  } else {
    static_cast<void>(std::printf("safety shell: never engaged\n"));
  }

  // Layer-by-layer counterfactual (closed-form constants of the scenario):
  static_cast<void>(std::printf(  // NOLINT(concurrency-mt-unsafe)
      "\ncounterfactuals (closed-form):\n"
      "  net alone            -> impact at ~%.0f m/s (depth bias keeps cruise)\n"
      "  net + advisory       -> impact at ~%.0f m/s (cap holds, bias persists)\n"
      "  net + advisory+shell -> stopped %.1f m before the obstacle\n",
      kCruise, kYieldCap, stop_distance));

  static_cast<void>(std::printf(  // NOLINT(concurrency-mt-unsafe)
      "final: v=%.2f m/s (margin >= %.1f m required)\n", last.v, kMargin));
  // The full loop lineage grows without bound (feedback provenance);
  // show the head of the first branch and the branch count.
  static_cast<void>(std::printf("last chassis lineage (%zu branches, head shown):\n  %.240s%s\n",
                                last.branches, last.lineage.c_str(),
                                last.lineage.size() > 240 ? "..." : ""));

  const bool stopped = last.v < 0.2;
  const bool clear = stop_distance >= kMargin;
  const bool advisory_ok = yield_frame != nullptr;
  const bool verdict = stopped && clear && stats.shell_ticks > 0 && advisory_ok;
  static_cast<void>(std::printf("\nverdict: %s (stopped=%s, clear=%s, shell=%s, advisory=%s)\n",
                                verdict ? "PASS" : "FAIL", stopped ? "yes" : "no",
                                clear ? "yes" : "no", stats.shell_ticks > 0 ? "yes" : "no",
                                advisory_ok ? "yes" : "no"));
  return verdict ? 0 : 2;
} catch (const std::exception& e) {
  static_cast<void>(std::fprintf(stderr, "error: %s\n", e.what()));
  return 1;
}
