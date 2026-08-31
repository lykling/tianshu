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

#include "tianshu/core/lineage.h"

#include <cstddef>
#include <string>

namespace tianshu::core {
namespace {

void append_hop(std::string* out, const LineageHop& hop) {
  *out += hop.channel;
  *out += '#';
  *out += std::to_string(hop.seq);
}

}  // namespace

void Lineage::merge(const Lineage& other) {
  for (const Branch& incoming : other.branches_) {
    bool replaced = false;
    for (Branch& existing : branches_) {
      if (existing.root.channel == incoming.root.channel) {
        if (incoming.hops.size() > existing.hops.size()) {
          existing = incoming;
        }
        replaced = true;
        break;
      }
    }
    if (!replaced && branches_.size() < kMaxBranches) {
      branches_.push_back(incoming);
    }
  }
}

std::string Lineage::describe() const {
  std::string out;
  for (std::size_t b = 0; b < branches_.size(); ++b) {
    if (b != 0) {
      out += " | ";
    }
    append_hop(&out, branches_[b].root);
    for (const LineageHop& hop : branches_[b].hops) {
      out += " -> ";
      append_hop(&out, hop);
    }
  }
  return out;
}

}  // namespace tianshu::core
