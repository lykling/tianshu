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

// Cross-process SHM listener: subscribes to ImuData on /sensing/imu.
//
// Run together with shm_talker in two terminals:
//   ./shm_talker          # terminal 1
//   ./shm_listener        # terminal 2 (start order does not matter)

#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>

#include "tianshu/core/message_traits.h"
#include "tianshu/core/node.h"

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
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  tianshu::core::Node node(tianshu::transport::TransportMode::kShm);
  auto reader = node.create_typed_reader<ImuData>("/sensing/imu");
  if (reader == nullptr) {
    std::fprintf(stderr, "shm_listener: failed to create reader\n");
    return 1;
  }

  std::printf("shm_listener: subscribed to ImuData on /sensing/imu (Ctrl-C to stop)\n");

  std::uint64_t last_seq = 0;
  bool have_last = false;
  std::uint64_t received = 0;
  std::uint64_t gaps = 0;

  while (g_stop == 0) {
    if (const ImuData* imu = reader->try_fetch(); imu != nullptr) {
      const std::uint64_t seq = reader->last_seq();
      if (!have_last || seq != last_seq) {
        if (have_last && seq > last_seq + 1) {
          gaps += seq - last_seq - 1;
        }
        last_seq = seq;
        have_last = true;
        ++received;
        if (received % 10 == 1) {
          std::printf("shm_listener: seq=%llu ts=%.1f az=%.2f\n",
                      static_cast<unsigned long long>(seq), imu->timestamp, imu->az);
        }
      }
    }
    usleep(1000);
  }

  std::printf("shm_listener: %llu messages, %llu dropped\n",
              static_cast<unsigned long long>(received), static_cast<unsigned long long>(gaps));
  return 0;
}
