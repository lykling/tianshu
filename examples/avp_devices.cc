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

// AVP device library (ADR-0025 closed-loop demo): drivers and the chassis
// as REGISTERED components. Written once, self-contained; any flow (or
// DAG) references them by name via from() — this translation unit never
// knows which flow consumes it.

#include <cstdint>
#include <string>
#include <utility>

#include "tianshu/core/component.h"
#include "tianshu/core/message_traits.h"

#include "avp_types.h"

TIANSHU_TRAITS_POD(RadarMsg, "demo.RadarMsg");
TIANSHU_TRAITS_POD(GnssMsg, "demo.GnssMsg");
TIANSHU_TRAITS_POD(ChassisState, "demo.ChassisState");
TIANSHU_TRAITS_POD(ControlCmd, "demo.ControlCmd");

namespace {

class RadarDriverBase : public tianshu::core::TimerSourceComponent<RadarMsg> {
 public:
  RadarDriverBase(std::string name, std::uint32_t id, std::uint32_t base_targets)
      : TimerSourceComponent(std::move(name)), id_(id), base_targets_(base_targets) {}

 protected:
  void proc() override {
    publish(RadarMsg{.t = tick_,
                     .radar_id = id_,
                     .n_targets = base_targets_ + static_cast<std::uint32_t>(tick_ % 3)});
    ++tick_;
  }

  [[nodiscard]] std::string_view out_channel() const override { return {}; }

 private:
  std::uint32_t id_;
  std::uint32_t base_targets_;
  std::uint64_t tick_{0};
};

class GnssDriver final : public tianshu::core::TimerSourceComponent<GnssMsg> {
 public:
  explicit GnssDriver(std::string name) : TimerSourceComponent(std::move(name)) {}

 protected:
  void proc() override {
    publish(GnssMsg{.t = tick_,
                    .x = 100.0 + (static_cast<double>(tick_) * 0.5),
                    .y = -12.0,
                    .heading = 0.02 * static_cast<double>(tick_ % 10)});
    ++tick_;
  }

  [[nodiscard]] std::string_view out_channel() const override { return {}; }

 private:
  std::uint64_t tick_{0};
};

// The chassis as one registered component: reads commands, integrates
// speed, publishes state. init() publishes the power-on report — through
// the from() bridge this runs after all wiring, so feedback loops
// bootstrap with one honest initial message (no seed source).
class ChassisComponent final : public tianshu::core::Component<ControlCmd, ChassisState> {
 public:
  explicit ChassisComponent(std::string name) : Component(std::move(name)) {}

  bool init() override {
    publish(ChassisState{.t = 0, .speed = 0.0, .steer = 0.0});
    return true;
  }

 protected:
  void proc(const ControlCmd& cmd) override {
    speed_ += cmd.accel * 0.05;
    publish(ChassisState{.t = cmd.t, .speed = speed_, .steer = cmd.steer_cmd});
  }

  [[nodiscard]] std::string_view out_channel() const override { return {}; }

 private:
  double speed_{0.0};
};

class RadarFrontDriver final : public RadarDriverBase {
 public:
  explicit RadarFrontDriver(std::string name) : RadarDriverBase(std::move(name), 0, 2) {}
};

class RadarRearDriver final : public RadarDriverBase {
 public:
  explicit RadarRearDriver(std::string name) : RadarDriverBase(std::move(name), 1, 1) {}
};

}  // namespace

TIANSHU_REGISTER_COMPONENT(RadarFrontDriver, "avp.radar.front")
TIANSHU_REGISTER_COMPONENT(RadarRearDriver, "avp.radar.rear")
TIANSHU_REGISTER_COMPONENT(GnssDriver, "avp.gnss")
TIANSHU_REGISTER_COMPONENT(ChassisComponent, "avp.chassis")
