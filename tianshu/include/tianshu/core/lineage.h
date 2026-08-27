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

// Lineage v0: per-message data lineage, cascaded automatically through
// the DSL map chain (per ADR-0022).
//
// Model:
//   - A message's Lineage = the root hop (source channel + seq) plus the
//     chain of input hops of every stage that produced it
//   - Sources create the root; each map appends its input hop
//   - v0 travels as a side record keyed by channel (single-writer FIFO
//     channels); L4-TRANS Message.lineage_ptr carries it in-band later

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tianshu::core {

struct LineageHop {
  std::string channel;
  std::uint64_t seq{0};
};

class Lineage {
 public:
  Lineage() = default;

  static Lineage rooted(std::string channel, std::uint64_t seq) {
    Lineage lin;
    lin.root_ = LineageHop{.channel = std::move(channel), .seq = seq};
    return lin;
  }

  void add_hop(LineageHop hop) { hops_.push_back(std::move(hop)); }

  [[nodiscard]] const LineageHop& root() const { return root_; }
  [[nodiscard]] const std::vector<LineageHop>& hops() const { return hops_; }

  // "demo/imu#42 -> demo/~0#42"
  [[nodiscard]] std::string describe() const;

 private:
  LineageHop root_;
  std::vector<LineageHop> hops_;
};

}  // namespace tianshu::core
