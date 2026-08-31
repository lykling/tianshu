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

// Lineage v0.5: per-message data lineage, cascaded automatically through
// the DSL chain (per ADR-0022, amended: branch model).
//
// Model:
//   - A message's Lineage = one or more derivation branches; each branch
//     is a root hop (source channel + seq) plus the hops of the stages
//     that produced it along that path
//   - Sources create the root; each map/join stage appends its output hop
//   - A join merges both parents' branch sets (DAG provenance); a linear
//     chain is the single-branch special case and renders exactly like
//     v0 ("a#1 -> b#2")
//   - Travels through per-consumer mailboxes in the DSL runtime (v0.5);
//     L4-TRANS Message.lineage_ptr carries it in-band in a later phase

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
  struct Branch {
    LineageHop root;
    std::vector<LineageHop> hops;
  };

  Lineage() = default;

  static Lineage rooted(std::string channel, std::uint64_t seq) {
    Lineage lin;
    lin.branches_.push_back(
        Branch{.root = LineageHop{.channel = std::move(channel), .seq = seq}, .hops = {}});
    return lin;
  }

  // Appends the hop to every branch (a fused stage derives from all of
  // its parents, so the new hop closes each branch).
  void add_hop(const LineageHop& hop) {
    for (Branch& b : branches_) {
      b.hops.push_back(hop);
    }
  }

  // Union of both branch sets (join provenance).
  void merge(const Lineage& other) {
    for (const Branch& b : other.branches_) {
      branches_.push_back(b);
    }
  }

  [[nodiscard]] bool empty() const { return branches_.empty(); }

  // Single-branch accessors (linear chains; v0-compatible surface).
  [[nodiscard]] const LineageHop& root() const {
    static const LineageHop EMPTY;
    return branches_.empty() ? EMPTY : branches_.front().root;
  }

  [[nodiscard]] const std::vector<LineageHop>& hops() const {
    static const std::vector<LineageHop> EMPTY;
    return branches_.empty() ? EMPTY : branches_.front().hops;
  }

  [[nodiscard]] const std::vector<Branch>& branches() const { return branches_; }

  // Linear: "demo/imu#42 -> demo/~0#42".
  // Joined:  "a#1 -> demo/~0#3 | b#0 -> demo/~0#3".
  [[nodiscard]] std::string describe() const;

 private:
  std::vector<Branch> branches_;
};

}  // namespace tianshu::core
