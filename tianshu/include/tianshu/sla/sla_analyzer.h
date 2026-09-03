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

// SLA compilation v0 (ADR-0029): load-time end-to-end deadline
// verification and budget allocation over the traced flow graph.
//
// Two-layer honest model:
//   Layer 1 (deterministic): per-endpoint worst path latency
//     L = max over backtracked paths of (sum of node WCETs + hops * c_hop)
//   Layer 2 (admission): utilization
//     U = sum over periodic sources of L_source_max / T_source
//     must satisfy U <= machine_cores * (1 - margin)
// No hard real-time claim is made on CFS; fixed-priority RTA is a
// Phase 2 upgrade once static scheduling exists.

#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace tianshu::sla {

// Typed SLA declaration attached to a channel (endpoint semantics,
// ADR-0029 D1: the deadline belongs to a channel, not the whole flow).
struct Sla {
  std::chrono::microseconds deadline{0};
};

// Analysis tuning. Values mirror ADR-0029 D2/D3 defaults; deployment
// overrides arrive from flow-adjacent configuration.
struct SlaConfig {
  std::chrono::microseconds default_wcet{std::chrono::microseconds(100)};
  std::chrono::microseconds hop_cost{std::chrono::microseconds(20)};
  unsigned machine_cores{0};  // 0 -> hardware_concurrency at analyze time
  double margin{0.2};
  bool strict_utilization{false};
};

// Analysis input graph: already type-erased, so the analyzer stays
// independent of the DSL layer.
struct SlaSource {
  std::string channel;
  std::chrono::milliseconds interval{};  // <=0 = aperiodic: no U term
};

struct SlaNode {
  std::string kind;  // "map" / "join" / "op" / "stateful" / "span" / "from"
  std::vector<std::string> in_channels;
  std::string out_channel;
  std::chrono::microseconds wcet{0};  // 0 = undeclared -> default + note
};

struct SlaEndpoint {
  std::string channel;
  std::chrono::microseconds deadline;
};

struct SlaViolation {
  std::string endpoint;
  std::chrono::microseconds deadline{};
  std::chrono::microseconds planned{};
  std::vector<std::string> path;  // source channel ... endpoint channel
  std::string worst_offender;     // out channel with the largest wcet share
};

struct SlaBudget {
  std::string channel;  // node out channel on the critical path
  std::chrono::microseconds planned{};
};

struct SlaReport {
  bool ok{true};
  std::vector<SlaViolation> violations;
  std::string saturation_warning;  // empty when layer 2 passes
  std::vector<std::string> default_wcet_notes;
  std::vector<SlaBudget> budgets;

  [[nodiscard]] std::string format() const;  // multi-line human summary
};

class SlaAnalyzer {
 public:
  // Throws std::invalid_argument on structurally invalid input
  // (unknown endpoint channel). Pure function otherwise.
  [[nodiscard]] static SlaReport analyze(const std::vector<SlaSource>& sources,
                                         const std::vector<SlaNode>& nodes,
                                         const std::vector<SlaEndpoint>& endpoints,
                                         const SlaConfig& config);
};

}  // namespace tianshu::sla
