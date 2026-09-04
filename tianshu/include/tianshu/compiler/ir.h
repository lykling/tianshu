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

// Compiler IR (ADR-0030 D2): the traced declaration graph promoted to
// the compiler's single intermediate representation. The same structure
// feeds the SLA pass (already consumed via the analyzer), the cache key
// (normalized hash), and the .dag/.conf exporters.

#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "tianshu/dsl/flow.h"
#include "tianshu/sla/sla_analyzer.h"

namespace tianshu::compiler {

// One graph node: a DSL declaration lowered to channel edges. Kinds:
// source / map / join / op / stateful / span / from / sink.
struct IrNode {
  std::string kind;
  std::vector<std::string> inputs;    // input channels; empty for sources
  std::string output;                 // output channel; empty for sinks
  std::string type_name;              // payload type, best effort
  std::chrono::microseconds wcet{0};  // declared; 0 = undeclared
  std::size_t topo_order{0};
};

class IrGraph {
 public:
  // Lowers a built Flow (with its declared WCETs, endpoints and SLA
  // report) into IR form. Pure function.
  [[nodiscard]] static IrGraph from_flow(const dsl::Flow& flow);

  // Assigns topological orders (Kahn; ties broken lexicographically by
  // output channel so the result is a pure function of the graph).
  // Cycle members (map_to feedback) keep deterministic trailing order.
  void compute_topo();

  // Canonical form for caching: anonymous channels are renumbered in
  // canonical traversal order and nodes are sorted, so two flows that
  // declare the same graph in different builder-call order normalize to
  // identical representations.
  void normalize();

  // Stable 64-bit FNV-1a over the normalized serialization; cache key
  // for generated artifacts (ADR-0030 D1).
  [[nodiscard]] std::string stable_hash() const;

  // .dag export: topology + channel table (TOML style, ADR-0016).
  [[nodiscard]] std::string export_dag() const;

  // .conf export: SLA verdict, budget table, artifact hash.
  [[nodiscard]] std::string export_conf() const;

  [[nodiscard]] const std::string& flow_name() const { return flow_name_; }
  [[nodiscard]] const std::vector<IrNode>& nodes() const { return nodes_; }
  [[nodiscard]] const std::vector<sla::SlaEndpoint>& endpoints() const { return endpoints_; }
  [[nodiscard]] const sla::SlaReport& sla() const { return sla_; }

 private:
  friend class IrGraphBuilder;
  std::string flow_name_;
  std::vector<IrNode> nodes_;
  std::vector<sla::SlaEndpoint> endpoints_;
  sla::SlaReport sla_;
};

}  // namespace tianshu::compiler
