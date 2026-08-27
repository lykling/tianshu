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

// Launcher: assembles a component DAG from config and runs it.
//
// Design (per L4-MAIN-1, ADR-0002 terminology: tianshu-launch, not mainboard):
//   - DagConfig: INI-style [component <name>] sections with type / inputs /
//     interval_ms. The real parser arrives with ADR-0025 (TOML); this
//     grammar is a deliberate Phase 1 subset parsed in ~100 dependency-free
//     lines, and TOML's [section] + key = value shape maps onto it directly.
//   - Launcher: create via ComponentFactory -> init() -> launch(node, ...),
//     SIGINT/SIGTERM -> shutdown() in reverse start order.

#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tianshu/core/component.h"
#include "tianshu/core/node.h"
#include "tianshu/transport/transport_backend.h"

namespace tianshu::core {

struct DagComponentConfig {
  std::string name;
  std::string type;
  std::vector<std::string> input_channels;
  std::chrono::milliseconds interval{0};
};

struct DagParseResult {
  std::vector<DagComponentConfig> components;
  std::string error;
  [[nodiscard]] bool ok() const { return error.empty(); }
};

class DagConfig {
 public:
  static DagParseResult parse(std::string_view text);
  static DagParseResult parse_file(const std::string& path);
};

class Launcher {
 public:
  explicit Launcher(transport::TransportMode mode = transport::TransportMode::kIntra);

  // Creates + init() + launch() every component. Returns false (with error
  // in `error`) without side effects on already-started components failing
  // later in the list: started ones are shut down before returning.
  bool start(const DagParseResult& dag, std::string* error);

  // Blocks until SIGINT/SIGTERM, then shuts down.
  void run_until_signal();

  void stop();

  [[nodiscard]] const std::vector<std::unique_ptr<ComponentBase>>& components() const {
    return components_;
  }

 private:
  transport::TransportMode mode_;
  std::unique_ptr<Node> node_;
  std::vector<std::unique_ptr<ComponentBase>> components_;
  std::vector<ComponentBase*> started_;
};

}  // namespace tianshu::core
