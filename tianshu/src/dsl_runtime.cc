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
#include <functional>
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
#include "tianshu/dsl/record.h"
#include "tianshu/dsl/record_v2.h"
#include "tianshu/sla/sla_stats.h"
#include "tianshu/transport/transport_backend.h"

namespace tianshu::dsl {

namespace {

// The message's own channel-local seq: publish paths always close every
// branch with a hop on the publishing channel (rooted for sources), so
// the last hop of the first branch — or the root when hopless — is it.
std::uint64_t own_seq_of(const core::Lineage& lin) {
  if (lin.branches().empty()) {
    return 0;
  }
  const auto& branch = lin.branches().front();
  return branch.hops.empty() ? branch.root.seq : branch.hops.back().seq;
}

}  // namespace

void FlowRuntime::publish_bytes(const std::string& channel, const void* data, std::size_t size,
                                const core::Lineage& lineage) {
  // SLA runtime defense (ADR-0029 D6): the v0 cascade is synchronous
  // within one source tick, so a thread-local birth timestamp carries
  // the e2e origin exactly. Flows without SLA endpoints pay one null
  // check; uninstrumented channels pay a map miss.
  static thread_local std::uint64_t sla_born_ns = 0;
  const bool sla_outermost = sla_stats_ != nullptr && sla_born_ns == 0;
  std::uint64_t sla_now_ns = 0;
  if (sla_stats_ != nullptr) {
    sla_now_ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                std::chrono::steady_clock::now().time_since_epoch())
                                                .count());
    if (sla_outermost) {
      sla_born_ns = sla_now_ns;
    }
  }

  {
    const std::scoped_lock lock(mutex_);
    const auto it = channel_queues_.find(channel);
    if (it != channel_queues_.end()) {
      for (detail::LineageMailbox* mailbox : it->second) {
        mailbox->push(lineage);
      }
    }
    histories_.try_emplace(channel, kHistoryDepth);
    histories_.at(channel).push(own_seq_of(lineage), data, size, lineage);

    // Live recording (ADR-0028 v2): capture in-flight with timestamp.
    if (recorder_ != nullptr) {
      const auto id_it = recorder_channel_ids_.find(channel);
      const std::uint16_t ch_id = id_it != recorder_channel_ids_.end()
                                      ? id_it->second
                                      : recorder_->add_channel(channel, 0, "");
      if (id_it == recorder_channel_ids_.end()) {
        recorder_channel_ids_[channel] = ch_id;
      }
      const auto now_ns =
          static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
      recorder_->append(ch_id, own_seq_of(lineage), now_ns, data, size, &lineage);
    }
  }
  core::DataDispatcher::instance().dispatch(core::channel_id_for(channel), data, size);

  if (sla_stats_ != nullptr) {
    sla_stats_->record_if_endpoint(channel, std::chrono::nanoseconds{sla_now_ns - sla_born_ns});
    if (sla_outermost) {
      sla_born_ns = 0;
    }
  }
}

const detail::HistoryRing* FlowRuntime::history(const std::string& channel) const {
  const std::scoped_lock lock(mutex_);
  const auto it = histories_.find(channel);
  return it == histories_.end() ? nullptr : &it->second;
}

std::vector<sla::SlaEndpointStats> FlowRuntime::sla_snapshot() const {
  return sla_stats_ != nullptr ? sla_stats_->snapshot() : std::vector<sla::SlaEndpointStats>{};
}

void FlowRuntime::start_recording(const std::string& path, record::Compression compression) {
  if (recorder_ != nullptr) {
    return;
  }
  recorder_ = std::make_unique<record::RecordWriter>(path, compression);
  recorder_channel_ids_.clear();
  recording_start_ts_ =
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

bool FlowRuntime::stop_recording() {
  if (recorder_ == nullptr) {
    return false;
  }
  const bool ok = recorder_->finish();
  recorder_.reset();
  recorder_channel_ids_.clear();
  return ok;
}

bool FlowRuntime::is_recording() const { return recorder_ != nullptr; }

bool FlowRuntime::record_to(const std::string& path) const {
  RecordFile file(path);
  const std::scoped_lock lock(mutex_);
  for (const auto& [channel, ring] : histories_) {
    for (const auto& entry : ring.entries()) {
      const RecordedMessage rec{.channel = channel,
                                .seq = entry.seq,
                                .bytes = entry.bytes,
                                .lineage_text = entry.lineage.describe()};
      file.append(rec);
    }
  }
  return file.save();
}

void FlowRuntime::replay_from(const std::vector<RecordedMessage>& records) {
  // Records arrive in capture order, so a straight re-publish
  // reproduces the original cascade (slice query's offline substrate).
  for (const auto& rec : records) {
    publish_bytes(rec.channel, rec.bytes.data(), rec.bytes.size(),
                  core::Lineage::rooted(rec.channel, rec.seq));
  }
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
  // Lineage pairing (ADR-0025 correction): the mailbox pops 1:1 with the
  // component's FIFO consumption, so every publish inside proc carries
  // its triggering input's lineage as parent — the loop unrolls across
  // the component boundary.
  const auto mailbox = register_mailbox(in_channel);
  comp->set_input_lineage_provider([mailbox] { return mailbox->pop(); });
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
    // Component output with a parent lineage (published inside proc,
    // ADR-0025 correction): derive from it; lineage-free outputs (no
    // provider) and init-time publishes (empty parent) root at the
    // channel — same rule as publish_op.
    const auto* parent = static_cast<const core::Lineage*>(msg.lineage_ptr);
    if (parent != nullptr && !parent->empty()) {
      publish_derived(*parent, out_channel, msg.data, msg.size);
      return;
    }
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

namespace {

void drive_source(const Flow::SourceDecl& source, std::chrono::milliseconds duration,
                  std::chrono::steady_clock::time_point start, FlowRuntime* rt) {
  const auto interval = source.interval;
  std::uint64_t tick = 0;
  auto next = start + interval;
  while (std::chrono::steady_clock::now() - start < duration) {
    std::this_thread::sleep_until(next);
    if (std::chrono::steady_clock::now() - start >= duration) {
      return;
    }
    source.drive(*rt, tick);
    ++tick;
    next += interval;
  }
}

}  // namespace

void FlowRuntime::run_for(const Flow& flow, std::chrono::milliseconds duration) {
  if (!flow.sla_endpoints().empty() && sla_stats_ == nullptr) {
    sla_stats_ = std::make_unique<sla::SlaStatsCollector>();
    for (const auto& endpoint : flow.sla_endpoints()) {
      sla_stats_->add_endpoint(endpoint.channel, endpoint.deadline);
    }
  }
  for (const auto& map_decl : flow.maps()) {
    map_decl.wire(*this);
  }
  for (const auto& join_decl : flow.joins()) {
    join_decl.wire(*this);
  }
  for (const auto& op_decl : flow.ops()) {
    op_decl.wire(*this);
  }
  for (const auto& st_decl : flow.statefuls()) {
    st_decl.wire(*this);
  }
  for (const auto& span_decl : flow.spans()) {
    span_decl.wire(*this);
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
    source_threads.emplace_back(drive_source, std::cref(source), duration, start, this);
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
