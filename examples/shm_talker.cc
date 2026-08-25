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

// Cross-process SHM talker: publishes IMU-like POD messages on /sensing/imu.
//
// Run together with shm_listener in two terminals:
//   ./shm_talker          # terminal 1
//   ./shm_listener        # terminal 2 (start order does not matter)

#include <unistd.h>

#include <csignal>
#include <cstdint>
#include <cstdio>

#include "tianshu/core/message_traits.h"
#include "tianshu/core/node.h"
#include "tianshu/transport/transport_backend.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void handle_signal([[maybe_unused]] int sig) { g_stop = 1; }

struct ImuData {
  double timestamp;
  double ax;
  double ay;
  double az;
  double gx;
  double gy;
  double gz;
};

}  // namespace

TIANSHU_TRAITS_POD(ImuData, "tianshu.example.ImuData");

int main() {
  static_cast<void>(std::signal(SIGINT, handle_signal));
  static_cast<void>(std::signal(SIGTERM, handle_signal));

  tianshu::core::Node node(tianshu::transport::TransportMode::kShm);
  auto writer = node.create_typed_writer<ImuData>("/sensing/imu");
  if (writer == nullptr) {
    static_cast<void>(std::fprintf(stderr, "shm_talker: failed to create writer\n"));
    return 1;
  }

  std::printf("shm_talker: publishing ImuData on /sensing/imu (10 Hz, Ctrl-C to stop)\n");

  std::uint64_t seq = 0;
  while (g_stop == 0) {
    const ImuData imu{
        .timestamp = static_cast<double>(seq) * 0.1,
        .ax = 0.01,
        .ay = -0.02,
        .az = 9.81,
        .gx = 0.001,
        .gy = 0.002,
        .gz = 0.003,
    };
    writer->write(imu);
    if (seq % 10 == 0) {
      std::printf("shm_talker: seq=%llu ts=%.1f az=%.2f\n", static_cast<unsigned long long>(seq),
                  imu.timestamp, imu.az);
    }
    ++seq;
    usleep(100 * 1000);
  }

  std::printf("shm_talker: stopped after %llu messages\n", static_cast<unsigned long long>(seq));
  return 0;
}
