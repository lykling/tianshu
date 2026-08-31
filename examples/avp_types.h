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

// AVP demo message types shared by the device library and the flows.

#pragma once

#include <cstdint>

struct RadarMsg {
  std::uint64_t t;
  std::uint32_t radar_id;
  std::uint32_t n_targets;
};

struct ObstacleList {
  std::uint64_t t;
  std::uint32_t n_obstacles;
};

struct GnssMsg {
  std::uint64_t t;
  double x;
  double y;
  double heading;
};

struct PerceptionOut {
  std::uint64_t t;
  std::uint32_t n_obstacles;
  double pose_x;
  double pose_y;
};

struct PredictionOut {
  std::uint64_t t;
  std::uint32_t n_obstacles;
  double risk;
};

struct ChassisState {
  std::uint64_t t;
  double speed;
  double steer;
};

struct Plan {
  std::uint64_t t;
  double target_speed;
  double current_speed;
  double curvature;
};

struct ControlCmd {
  std::uint64_t t;
  double accel;
  double steer_cmd;
};
