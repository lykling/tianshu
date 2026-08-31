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
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "tianshu/core/component.h"
#include "tianshu/core/data_dispatcher.h"
#include "tianshu/core/data_visitor.h"
#include "tianshu/core/lineage.h"
#include "tianshu/core/node.h"
#include "tianshu/dsl/flow.h"
#include "tianshu/transport/transport_backend.h"

namespace tianshu::dsl {

void FlowRuntime::publish_bytes(const std::string& channel, const void* data, std::size_t size,
                                const core::Lineage& lineage) {
  {
    const std::scoped_lock lock(mutex_);
    const auto it = channel_queues_.find(channel);
    if (it != channel_queues_.end()) {
      for (detail::LineageMailbox* mailbox : it->second) {
        mailbox->push(lineage);
      }
    }
  }
  core::DataDispatcher::instance().dispatch(core::channel_id_for(channel), data, size);
}

std::shared_ptr<detail::LineageMailbox> FlowRuntime::register_mailbox(const std::string& channel) {
  auto mailbox = std::make_shared<detail::LineageMailbox>(kQueueDepth * 2);
  const std::scoped_lock lock(mutex_);
  channel_queues_[channel].push_back(mailbox.get());
  mailboxes_.push_back(mailbox);
  return mailbox;
}

FlowRuntime::~FlowRuntime() {
  for (const auto& comp : components_) {
    comp->shutdown();
  }
}

void FlowRuntime::attach_referenced_source(const std::string& registry_name,
                                           const std::string& out_channel,
                                           std::chrono::milliseconds interval) {
  auto comp = core::ComponentFactory::instance().create(registry_name, registry_name);
  if (comp == nullptr) {
    return;
  }
  if (bridge_node_ == nullptr) {
    bridge_node_ = std::make_unique<core::Node>(transport::TransportMode::kIntra);
  }
  comp->set_out_channel_override(out_channel);
  if (!comp->launch(*bridge_node_, {}, interval)) {
    return;
  }
  attach_bridge_reader(out_channel);
  components_.emplace_back(std::move(comp));
  const auto held = components_.back();
  init_hooks_.emplace_back([held] { held->init(); });
}

void FlowRuntime::attach_referenced_component(const std::string& registry_name,
                                              const std::string& in_channel,
                                              const std::string& out_channel) {
  auto comp = core::ComponentFactory::instance().create(registry_name, registry_name);
  if (comp == nullptr) {
    return;
  }
  if (bridge_node_ == nullptr) {
    bridge_node_ = std::make_unique<core::Node>(transport::TransportMode::kIntra);
  }
  comp->set_out_channel_override(out_channel);
  if (!comp->launch(*bridge_node_, {in_channel}, {})) {
    return;
  }
  attach_bridge_reader(out_channel);
  components_.emplace_back(std::move(comp));
  const auto held = components_.back();
  init_hooks_.emplace_back([held] { held->init(); });
}

void FlowRuntime::publish_derived(const core::Lineage& parent, const std::string& channel,
                                  const void* data, std::size_t size) {
  core::Lineage lin = parent;
  lin.add_hop({.channel = channel, .seq = next_seq(channel)});
  publish_bytes(channel, data, size, lin);
}

void FlowRuntime::attach_bridge_reader(const std::string& out_channel) {
  auto reader = bridge_node_->create_reader(out_channel);
  reader->set_callback([this, out_channel](const transport::Message& msg) {
    publish_bytes(out_channel, msg.data, msg.size,
                  core::Lineage::rooted(out_channel, next_seq(out_channel)));
  });
  bridge_readers_.push_back(std::move(reader));
}

void FlowRuntime::publish_op(const std::string& channel, const void* data, std::size_t size,
                             const core::Lineage& parent) {
  if (parent.empty()) {
    publish_bytes(channel, data, size, core::Lineage::rooted(channel, next_seq(channel)));
    return;
  }
  publish_derived(parent, channel, data, size);
}

std::uint64_t FlowRuntime::next_seq(const std::string& channel) {
  const std::scoped_lock lock(mutex_);
  return seq_counters_[channel]++;
}

void FlowRuntime::run_for(const Flow& flow, std::chrono::milliseconds duration) {
  for (const auto& map_decl : flow.maps()) {
    map_decl.wire(*this);
  }
  for (const auto& join_decl : flow.joins()) {
    join_decl.wire(*this);
  }
  for (const auto& op_decl : flow.ops()) {
    op_decl.wire(*this);
  }
  for (const auto& from_decl : flow.froms()) {
    from_decl.wire(*this);
  }
  for (const auto& sink_decl : flow.sinks()) {
    sink_decl.wire(*this);
  }
  // Bootstrap publications LAST: every consumer mailbox of a box output
  // channel is registered by the time on_init fires (ADR-0024).
  for (const auto& hook : init_hooks_) {
    hook();
  }

  const auto start = std::chrono::steady_clock::now();
  std::vector<std::thread> source_threads;
  source_threads.reserve(flow.sources().size());
  for (const auto& source : flow.sources()) {
    source_threads.emplace_back([&source, duration, start, this] {
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
  // Referenced timer components drive themselves on their own threads;
  // the runtime stays alive for the full duration so they are not torn
  // down early (pure-DSL flows have already waited via the joins).
  if (!components_.empty()) {
    std::this_thread::sleep_until(start + duration);
    // Quiesce BEFORE returning: joining the timer threads drains every
    // in-flight cascade, so teardown never races a running callback.
    for (const auto& comp : components_) {
      comp->quiesce();
    }
  }
}

}  // namespace tianshu::dsl
