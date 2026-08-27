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

// ti-launch: assemble a component DAG from a config file and run it.
//
//   ti-launch <flow.dag> [--mode intra|shm]
//   ti launch <flow.dag>          (via the ti dispatcher)

#include <cstdio>
#include <string>

#include "tianshu/core/launcher.h"
#include "tianshu/transport/transport_backend.h"

namespace {

void print_usage() {
  static_cast<void>(std::fprintf(stderr, "usage: ti-launch <flow.dag> [--mode intra|shm]\n"));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage();
    return 2;
  }

  std::string dag_path;
  auto mode = tianshu::transport::TransportMode::kIntra;
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (arg == "--mode" && i + 1 < argc) {
      const std::string value(
          argv[++i]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      if (value == "shm") {
        mode = tianshu::transport::TransportMode::kShm;
      } else if (value != "intra") {
        static_cast<void>(
            std::fprintf(stderr, "ti-launch: unknown mode '%s' (intra|shm)\n", value.c_str()));
        return 2;
      }
    } else if (!dag_path.empty() || arg.starts_with("--")) {
      print_usage();
      return 2;
    } else {
      dag_path = arg;
    }
  }
  if (dag_path.empty()) {
    print_usage();
    return 2;
  }

  const auto dag = tianshu::core::DagConfig::parse_file(dag_path);
  if (!dag.ok()) {
    static_cast<void>(
        std::fprintf(stderr, "ti-launch: %s: %s\n", dag_path.c_str(), dag.error.c_str()));
    return 1;
  }

  tianshu::core::Launcher launcher(mode);
  std::string error;
  if (!launcher.start(dag, &error)) {
    static_cast<void>(std::fprintf(stderr, "ti-launch: %s\n", error.c_str()));
    return 1;
  }

  static_cast<void>(std::printf("ti-launch: %zu components running (Ctrl-C to stop)\n",
                                launcher.components().size()));
  launcher.run_until_signal();
  static_cast<void>(std::printf("ti-launch: stopped\n"));
  return 0;
}
