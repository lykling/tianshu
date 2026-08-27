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

// DSL runtime v0: interprets a Flow declaration on the L4 stack
// (per ADR-0021). One consumer of the declaration; the L1 compiler
// replaces it later without touching the Flow API.
//
// Execution model (v0):
//   - Sources drive the DataDispatcher DIRECTLY (no transport); one
//     publish cascades the whole chain synchronously on the source
//     thread (see ADR-0021 amendment)
//   - Each map is a DataVisitor on its input channel; its fused
//     callback runs fn and publishes the output with the cascaded
//     lineage (per ADR-0022: side FIFO keyed by channel)

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "tianshu/core/data_visitor.h"
#include "tianshu/core/lineage.h"
#include "tianshu/dsl/flow.h"

namespace tianshu::dsl {

namespace detail {

class StageHolder {
 public:
  virtual ~StageHolder() = default;
};

template <typename T>
class VisitorStage : public StageHolder {
 public:
  explicit VisitorStage(std::unique_ptr<core::DataVisitor<T>> v) : visitor(std::move(v)) {}

  std::unique_ptr<core::DataVisitor<T>> visitor;
};

}  // namespace detail

class FlowRuntime {
 public:
  FlowRuntime() = default;
  ~FlowRuntime() = default;

  FlowRuntime(const FlowRuntime&) = delete;
  FlowRuntime& operator=(const FlowRuntime&) = delete;

  // Records `lineage` for the next message on `channel`, then cascades
  // the payload through the DataDispatcher (synchronous chain).
  void publish_bytes(const std::string& channel, const void* data, std::size_t size,
                     const core::Lineage& lineage);

  // Map stage wiring (called by Flow::MapDecl::wire).
  template <typename TIn, typename TOut>
  void attach_map(const std::string& in_channel, const std::string& out_channel,
                  std::function<TOut(const TIn&)> fn) {
    // The fused callback needs the visitor pointer, but the visitor owns
    // the callback; bridge through a shared box written before the first
    // dispatch can happen (wiring precedes source ticks).
    const auto box = std::make_shared<core::DataVisitor<TIn>*>(nullptr);
    auto visitor = std::make_unique<core::DataVisitor<TIn>>(
        in_channel, kQueueDepth, [this, box, in_channel, out_channel, fn = std::move(fn)] {
          auto* visitor_ptr = *box;
          if (visitor_ptr == nullptr) {
            return;
          }
          while (TIn* msg = visitor_ptr->try_fetch_0()) {
            const core::Lineage parent = pop_lineage(in_channel);
            TOut out = fn(*msg);
            publish_derived(parent, out_channel, &out, sizeof(TOut));
          }
        });
    *box = visitor.get();
    stages_.push_back(std::make_unique<detail::VisitorStage<TIn>>(std::move(visitor)));
  }

  // Sink wiring (called by Flow::SinkDecl::wire).
  template <typename T>
  void attach_sink(const std::string& channel,
                   std::function<void(const T&, const core::Lineage&)> fn) {
    const auto box = std::make_shared<core::DataVisitor<T>*>(nullptr);
    auto visitor = std::make_unique<core::DataVisitor<T>>(
        channel, kQueueDepth, [box, channel, fn = std::move(fn), this] {
          auto* visitor_ptr = *box;
          if (visitor_ptr == nullptr) {
            return;
          }
          while (T* msg = visitor_ptr->try_fetch_0()) {
            fn(*msg, pop_lineage(channel));
          }
        });
    *box = visitor.get();
    stages_.push_back(std::make_unique<detail::VisitorStage<T>>(std::move(visitor)));
  }

  // Interprets the flow: wires maps/sinks, then drives every source on
  // its interval (absolute-deadline pacing, no cumulative drift) until
  // `duration` elapses.
  void run_for(const Flow& flow, std::chrono::milliseconds duration);

 private:
  // Publishes with a lineage derived from `parent` (parent chain + this
  // channel's hop with a fresh per-channel seq).
  void publish_derived(const core::Lineage& parent, const std::string& channel, const void* data,
                       std::size_t size);

  // Oldest recorded lineage for `channel` (FIFO, aligned with the
  // visitor buffer order: single writer per channel, single consumer).
  [[nodiscard]] core::Lineage pop_lineage(const std::string& channel);

  [[nodiscard]] std::uint64_t next_seq(const std::string& channel);

  static constexpr std::size_t kQueueDepth = 16;

  std::vector<std::unique_ptr<detail::StageHolder>> stages_;

  std::mutex mutex_;
  std::unordered_map<std::string, std::deque<core::Lineage>> side_lineage_;
  std::unordered_map<std::string, std::uint64_t> seq_counters_;
};

namespace detail {

template <typename T>
std::function<void(FlowRuntime&, std::uint64_t)> make_source_drive(
    std::string channel, std::function<T(std::uint64_t)> emit) {
  return
      [channel = std::move(channel), emit = std::move(emit)](FlowRuntime& rt, std::uint64_t tick) {
        const T value = emit(tick);
        auto lin = core::Lineage::rooted(channel, tick);
        rt.publish_bytes(channel, &value, sizeof(T), lin);
      };
}

template <typename TIn, typename TOut>
std::function<void(FlowRuntime&)> make_map_wire(std::string in_channel, std::string out_channel,
                                                std::function<TOut(const TIn&)> fn) {
  return [in_channel = std::move(in_channel), out_channel = std::move(out_channel),
          fn = std::move(fn)](FlowRuntime& rt) {
    rt.template attach_map<TIn, TOut>(in_channel, out_channel, fn);
  };
}

template <typename T>
std::function<void(FlowRuntime&)> make_sink_wire(
    std::string channel, std::function<void(const T&, const core::Lineage&)> fn) {
  return [channel = std::move(channel), fn = std::move(fn)](FlowRuntime& rt) {
    rt.template attach_sink<T>(channel, fn);
  };
}

}  // namespace detail

}  // namespace tianshu::dsl
