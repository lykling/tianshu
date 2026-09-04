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

#include "tianshu/compiler/ir.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <ios>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "tianshu/dsl/flow.h"

namespace tianshu::compiler {
namespace {

[[nodiscard]] std::string fnv1a64(const std::string& data) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const char c : data) {
    hash ^= static_cast<std::uint64_t>(static_cast<std::uint8_t>(c));
    hash *= 1099511628211ULL;
  }
  std::ostringstream out;
  out << std::hex << hash;
  return out.str();
}

[[nodiscard]] bool is_anon(const std::string& channel) {
  return channel.find("/~") != std::string::npos;
}

// Fills per-node indegree (inputs produced inside the graph) and the
// producer -> consumers adjacency used by the topological walk.
using OutputIndex = std::unordered_map<std::string, std::size_t>;

void build_dependency_edges(const std::vector<IrNode>& nodes, const OutputIndex& node_by_output,
                            std::vector<std::size_t>& indegree,
                            std::vector<std::vector<std::size_t>>& consumers) {
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    std::set<std::size_t> seen;
    for (const auto& in : nodes[i].inputs) {
      const auto it = node_by_output.find(in);
      if (it != node_by_output.end() && seen.insert(it->second).second) {
        ++indegree[i];
        consumers[it->second].push_back(i);
      }
    }
  }
}

// Appends "a" and "b" separated by a sentinel that cannot appear in
// channel names, so serialization is unambiguous for hashing.
void feed(std::string& sink, const std::string& a, const std::string& b) {
  sink += a;
  sink += '\x1f';
  sink += b;
  sink += '\x1f';
}

}  // namespace

IrGraph IrGraph::from_flow(const dsl::Flow& flow) {
  IrGraph graph;
  graph.flow_name_ = flow.name();
  graph.endpoints_ = flow.sla_endpoints();
  graph.sla_ = flow.sla_report();
  const auto& wcets = flow.wcet_by_out();
  const auto wcet_of = [&wcets](const std::string& out) {
    const auto it = wcets.find(out);
    return it != wcets.end() ? it->second : std::chrono::microseconds{0};
  };

  graph.nodes_.reserve(flow.sources().size() + flow.maps().size() + flow.joins().size() +
                       flow.ops().size() + flow.statefuls().size() + flow.spans().size() +
                       flow.froms().size() + flow.sinks().size());
  for (const auto& s : flow.sources()) {
    graph.nodes_.push_back(IrNode{.kind = "source",
                                  .inputs = {},
                                  .output = s.channel,
                                  .type_name = s.type_name,
                                  .wcet = std::chrono::microseconds{0}});
  }
  for (const auto& m : flow.maps()) {
    graph.nodes_.push_back(IrNode{.kind = "map",
                                  .inputs = {m.in_channel},
                                  .output = m.out_channel,
                                  .type_name = m.out_type_name,
                                  .wcet = wcet_of(m.out_channel)});
  }
  for (const auto& j : flow.joins()) {
    graph.nodes_.push_back(IrNode{.kind = "join",
                                  .inputs = {j.in_channel_a, j.in_channel_b},
                                  .output = j.out_channel,
                                  .type_name = j.out_type_name,
                                  .wcet = wcet_of(j.out_channel)});
  }
  for (const auto& b : flow.ops()) {
    graph.nodes_.push_back(IrNode{.kind = "op",
                                  .inputs = {b.in_channel},
                                  .output = b.out_channel,
                                  .type_name = b.out_type_name,
                                  .wcet = wcet_of(b.out_channel)});
  }
  for (const auto& s : flow.statefuls()) {
    graph.nodes_.push_back(IrNode{.kind = "stateful",
                                  .inputs = {s.in_channel},
                                  .output = s.out_channel,
                                  .type_name = s.out_type_name,
                                  .wcet = wcet_of(s.out_channel)});
  }
  for (const auto& sp : flow.spans()) {
    graph.nodes_.push_back(IrNode{.kind = "span",
                                  .inputs = {sp.trig_channel, sp.data_channel},
                                  .output = sp.out_channel,
                                  .type_name = sp.out_type_name,
                                  .wcet = wcet_of(sp.out_channel)});
  }
  for (const auto& f : flow.froms()) {
    if (f.in_channel.empty()) {
      graph.nodes_.push_back(IrNode{.kind = "source",
                                    .inputs = {},
                                    .output = f.out_channel,
                                    .type_name = f.out_type_name,
                                    .wcet = std::chrono::microseconds{0}});
    } else {
      graph.nodes_.push_back(IrNode{.kind = "from",
                                    .inputs = {f.in_channel},
                                    .output = f.out_channel,
                                    .type_name = f.out_type_name,
                                    .wcet = wcet_of(f.out_channel)});
    }
  }
  for (const auto& s : flow.sinks()) {
    graph.nodes_.push_back(IrNode{.kind = "sink",
                                  .inputs = {s.channel},
                                  .output = std::string(),
                                  .type_name = s.type_name,
                                  .wcet = std::chrono::microseconds{0}});
  }

  graph.compute_topo();
  return graph;
}

void IrGraph::compute_topo() {
  std::unordered_map<std::string, std::size_t> node_by_output;
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    if (!nodes_[i].output.empty()) {
      node_by_output.emplace(nodes_[i].output, i);
    }
  }

  std::vector<std::size_t> indegree(nodes_.size(), 0);
  std::vector<std::vector<std::size_t>> consumers(nodes_.size());
  build_dependency_edges(nodes_, node_by_output, indegree, consumers);

  // Priority key = (joined inputs, output): tie-breaks must rest on
  // semantic anchors (named input channels), not on anonymous channel
  // numbers — those differ when the same graph is declared in another
  // builder-call order, which is exactly what normalize must absorb.
  const auto node_key = [](const IrNode& n) {
    std::string key;
    for (const auto& in : n.inputs) {
      key += in;
      key += ',';
    }
    key += n.output;
    return key;
  };
  using Entry = std::pair<std::string, std::size_t>;
  std::priority_queue<Entry, std::vector<Entry>, std::greater<>> ready;
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    if (indegree[i] == 0) {
      ready.emplace(node_key(nodes_[i]), i);
    }
  }

  std::size_t order = 0;
  std::vector<IrNode> ordered;
  ordered.reserve(nodes_.size());
  while (!ready.empty()) {
    const std::size_t idx = ready.top().second;
    ready.pop();
    nodes_[idx].topo_order = order++;
    ordered.push_back(nodes_[idx]);
    for (const std::size_t c : consumers[idx]) {
      if (--indegree[c] == 0) {
        ready.emplace(node_key(nodes_[c]), c);
      }
    }
  }

  // Feedback cycles (map_to write-backs) never reach zero indegree:
  // append them deterministically after the acyclic prefix.
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    if (indegree[i] != 0) {
      nodes_[i].topo_order = order++;
      ordered.push_back(nodes_[i]);
    }
  }
  nodes_ = std::move(ordered);
}

void IrGraph::normalize() {
  // Renumber anonymous channels in canonical (topo, output) order; named
  // channels keep their identity. Two flows declaring the same graph in
  // different builder-call order converge here.
  std::unordered_map<std::string, std::string> rename;
  const std::string prefix = flow_name_ + "/~";
  std::size_t counter = 0;
  for (auto& node : nodes_) {
    if (is_anon(node.output)) {
      std::ostringstream canon;
      canon << prefix << counter++;
      rename[node.output] = canon.str();
    }
  }
  const auto rewrite = [&rename](std::string& channel) {
    const auto it = rename.find(channel);
    if (it != rename.end()) {
      channel = it->second;
    }
  };
  for (auto& node : nodes_) {
    for (auto& in : node.inputs) {
      rewrite(in);
    }
    // Sort on the canonical names, not the pre-rename ones.
    std::ranges::sort(node.inputs);
    rewrite(node.output);
  }
  for (auto& e : endpoints_) {
    rewrite(e.channel);
  }

  std::ranges::sort(nodes_, [](const IrNode& a, const IrNode& b) {
    if (a.topo_order != b.topo_order) {
      return a.topo_order < b.topo_order;
    }
    if (a.kind != b.kind) {
      return a.kind < b.kind;
    }
    return a.output < b.output;
  });
  // Re-run topo on canonical names so ties break identically everywhere.
  compute_topo();
}

std::string IrGraph::stable_hash() const {
  std::string data;
  feed(data, "flow", flow_name_);
  for (const auto& n : nodes_) {
    feed(data, "n", n.kind);
    for (const auto& in : n.inputs) {
      feed(data, "i", in);
    }
    feed(data, "o", n.output);
    feed(data, "w", std::to_string(n.wcet.count()));
    feed(data, "t", std::to_string(n.topo_order));
  }
  for (const auto& e : endpoints_) {
    feed(data, "e", e.channel);
    feed(data, "d", std::to_string(e.deadline.count()));
  }
  return fnv1a64(data);
}

std::string IrGraph::export_dag() const {
  std::ostringstream out;
  out << "dag_version = 1\n";
  out << "flow = \"" << flow_name_ << "\"\n";
  out << "hash = \"" << stable_hash() << "\"\n";
  for (const auto& n : nodes_) {
    out << "\n[[node]]\n";
    out << "kind = \"" << n.kind << "\"\n";
    out << "topo = " << n.topo_order << "\n";
    if (!n.inputs.empty()) {
      out << "inputs = [";
      for (std::size_t i = 0; i < n.inputs.size(); ++i) {
        out << (i == 0 ? "\"" : ", \"") << n.inputs[i] << "\"";
      }
      out << "]\n";
    }
    if (!n.output.empty()) {
      out << "output = \"" << n.output << "\"\n";
    }
    if (!n.type_name.empty()) {
      out << "type = \"" << n.type_name << "\"\n";
    }
    out << "wcet_us = " << n.wcet.count() << "\n";
  }
  return out.str();
}

std::string IrGraph::export_conf() const {
  std::ostringstream out;
  out << "conf_version = 1\n";
  out << "flow = \"" << flow_name_ << "\"\n";
  out << "hash = \"" << stable_hash() << "\"\n";
  out << "sla_ok = " << (sla_.ok ? "true" : "false") << "\n";
  if (!sla_.saturation_warning.empty()) {
    out << "saturation = \"" << sla_.saturation_warning << "\"\n";
  }
  for (const auto& e : endpoints_) {
    out << "\n[[sla]]\n";
    out << "endpoint = \"" << e.channel << "\"\n";
    out << "deadline_us = " << e.deadline.count() << "\n";
  }
  for (const auto& b : sla_.budgets) {
    out << "\n[[budget]]\n";
    out << "channel = \"" << b.channel << "\"\n";
    out << "planned_us = " << b.planned.count() << "\n";
  }
  return out.str();
}

}  // namespace tianshu::compiler
