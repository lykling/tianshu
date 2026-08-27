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

#include "tianshu/dsl/dsl_runtime.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "tianshu/core/data_dispatcher.h"
#include "tianshu/core/data_visitor.h"
#include "tianshu/core/lineage.h"
#include "tianshu/dsl/flow.h"

namespace tianshu::dsl {

void FlowRuntime::publish_bytes(const std::string& channel, const void* data, std::size_t size,
                                const core::Lineage& lineage) {
  {
    const std::scoped_lock lock(mutex_);
    side_lineage_[channel].push_back(lineage);
    if (side_lineage_[channel].size() > kQueueDepth * 2) {
      side_lineage_[channel].pop_front();
    }
  }
  core::DataDispatcher::instance().dispatch(core::channel_id_for(channel), data, size);
}

void FlowRuntime::publish_derived(const core::Lineage& parent, const std::string& channel,
                                  const void* data, std::size_t size) {
  core::Lineage lin = parent;
  lin.add_hop({.channel = channel, .seq = next_seq(channel)});
  publish_bytes(channel, data, size, lin);
}

core::Lineage FlowRuntime::pop_lineage(const std::string& channel) {
  const std::scoped_lock lock(mutex_);
  auto it = side_lineage_.find(channel);
  if (it == side_lineage_.end() || it->second.empty()) {
    return {};
  }
  core::Lineage lin = std::move(it->second.front());
  it->second.pop_front();
  return lin;
}

std::uint64_t FlowRuntime::next_seq(const std::string& channel) {
  const std::scoped_lock lock(mutex_);
  return seq_counters_[channel]++;
}

void FlowRuntime::run_for(const Flow& flow, std::chrono::milliseconds duration) {
  for (const auto& map_decl : flow.maps()) {
    map_decl.wire(*this);
  }
  for (const auto& sink_decl : flow.sinks()) {
    sink_decl.wire(*this);
  }

  std::vector<std::thread> source_threads;
  source_threads.reserve(flow.sources().size());
  for (const auto& source : flow.sources()) {
    source_threads.emplace_back([&source, duration, this] {
      const auto start = std::chrono::steady_clock::now();
      const auto interval = source.interval;
      std::uint64_t tick = 0;
      auto next = start + interval;
      while (std::chrono::steady_clock::now() - start < duration) {
        std::this_thread::sleep_until(next);
        if (std::chrono::steady_clock::now() - start >= duration) {
          return;
        }
        source.drive(*this, tick);
        ++tick;
        next += interval;
      }
    });
  }
  for (auto& thread : source_threads) {
    thread.join();
  }
}

}  // namespace tianshu::dsl
