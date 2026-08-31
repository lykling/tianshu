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

// DSL runtime v0.5: interprets a Flow declaration on the L4 stack
// (per ADR-0021). One consumer of the declaration; the L1 compiler
// replaces it later without touching the Flow API.
//
// Execution model:
//   - Sources drive the DataDispatcher DIRECTLY (no transport); one
//     publish cascades the whole chain synchronously on the source
//     thread (ADR-0021 amendment)
//   - Each map is a DataVisitor on its input channel; each join is a
//     two-input DataVisitor with AllLatest fusion (fires when both
//     inputs are non-empty, consumes one of each)
//   - Lineage travels through PER-CONSUMER mailboxes (ADR-0022
//     amendment): publish fans a copy out to every stage registered on
//     the channel, so multiple consumers never steal from each other

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

// Box publish handle (ADR-0024): bound to one box's output channel.
// Valid ONLY inside on_init/handle invocations; publish derives lineage
// from the current input when called from handle (map semantics) and
// roots at the box channel when called from on_init (source semantics).
template <typename T>
class BoxPub {
 public:
  void publish(const T& msg);

 private:
  friend class FlowRuntime;

  BoxPub(FlowRuntime* rt, std::string channel) : rt_(rt), channel_(std::move(channel)) {}

  FlowRuntime* rt_;
  std::string channel_;
  core::Lineage parent_;
};

namespace detail {

class StageHolder {
 public:
  virtual ~StageHolder() = default;
};

template <typename... Ts>
class VisitorStage : public StageHolder {
 public:
  explicit VisitorStage(std::unique_ptr<core::DataVisitor<Ts...>> v) : visitor(std::move(v)) {}

  std::unique_ptr<core::DataVisitor<Ts...>> visitor;
};

// Bounded lineage mailbox: one per (stage, input channel). The publisher
// pushes a copy to every mailbox registered on the channel; the owning
// stage pops in its own consumption order.
class LineageMailbox {
 public:
  explicit LineageMailbox(std::size_t depth) : depth_(depth) {}

  void push(const core::Lineage& lineage) {
    const std::scoped_lock lock(mutex_);
    queue_.push_back(lineage);
    if (queue_.size() > depth_) {
      queue_.pop_front();
    }
  }

  core::Lineage pop() {
    const std::scoped_lock lock(mutex_);
    if (queue_.empty()) {
      return {};
    }
    core::Lineage lineage = std::move(queue_.front());
    queue_.pop_front();
    return lineage;
  }

 private:
  std::size_t depth_;
  std::mutex mutex_;
  std::deque<core::Lineage> queue_;
};

}  // namespace detail

class FlowRuntime {
 public:
  FlowRuntime() = default;
  ~FlowRuntime() = default;

  FlowRuntime(const FlowRuntime&) = delete;
  FlowRuntime& operator=(const FlowRuntime&) = delete;

  template <typename U>
  friend class BoxPub;

  // Copies `lineage` to every consumer mailbox on `channel`, then
  // cascades the payload through the DataDispatcher (synchronous chain).
  void publish_bytes(const std::string& channel, const void* data, std::size_t size,
                     const core::Lineage& lineage);

  // Map stage wiring (called by Flow::MapDecl::wire).
  template <typename TIn, typename TOut>
  void attach_map(const std::string& in_channel, const std::string& out_channel,
                  std::function<TOut(const TIn&)> fn) {
    const auto mailbox = register_mailbox(in_channel);
    const auto box = std::make_shared<core::DataVisitor<TIn>*>(nullptr);
    auto visitor = std::make_unique<core::DataVisitor<TIn>>(
        in_channel, kQueueDepth, [this, box, mailbox, out_channel, fn = std::move(fn)] {
          auto* visitor_ptr = *box;
          if (visitor_ptr == nullptr) {
            return;
          }
          while (TIn* msg = visitor_ptr->try_fetch_0()) {
            const core::Lineage parent = mailbox->pop();
            TOut out = fn(*msg);
            publish_derived(parent, out_channel, &out, sizeof(TOut));
          }
        });
    *box = visitor.get();
    stages_.push_back(std::make_unique<detail::VisitorStage<TIn>>(std::move(visitor)));
  }

  // Join stage wiring (called by Flow::JoinDecl::wire): AllLatest fusion
  // over both inputs; the output lineage merges both parents' branches.
  template <typename TA, typename TB, typename TC>
  void attach_join(const std::string& in_a, const std::string& in_b, const std::string& out_channel,
                   std::function<TC(const TA&, const TB&)> fn) {
    const auto mailbox_a = register_mailbox(in_a);
    const auto mailbox_b = register_mailbox(in_b);
    const auto box = std::make_shared<core::DataVisitor<TA, TB>*>(nullptr);
    auto visitor = std::make_unique<core::DataVisitor<TA, TB>>(
        in_a, in_b, kQueueDepth,
        [this, box, mailbox_a, mailbox_b, out_channel, fn = std::move(fn)] {
          auto* visitor_ptr = *box;
          if (visitor_ptr == nullptr) {
            return;
          }
          TA* a = visitor_ptr->try_fetch_0();
          TB* b = visitor_ptr->try_fetch_1();
          if (a == nullptr || b == nullptr) {
            return;
          }
          core::Lineage merged = mailbox_a->pop();
          merged.merge(mailbox_b->pop());
          TC out = fn(*a, *b);
          publish_derived(merged, out_channel, &out, sizeof(TC));
        });
    *box = visitor.get();
    stages_.push_back(std::make_unique<detail::VisitorStage<TA, TB>>(std::move(visitor)));
  }

  // Box wiring (called by Flow::BoxDecl::wire): visitor on the input
  // channel + deferred on_init (runs after ALL wiring so consumers of the
  // output channel are registered before the bootstrap publication).
  template <typename TIn, typename TOut, typename TBox>
  void attach_box(const std::string& in_channel, const std::string& out_channel, TBox impl) {
    const std::shared_ptr<BoxPub<TOut>> pub(new BoxPub<TOut>(this, out_channel));
    const auto mailbox = register_mailbox(in_channel);
    const auto box_impl = std::make_shared<TBox>(std::move(impl));
    const auto stage = std::make_shared<core::DataVisitor<TIn>*>(nullptr);
    auto visitor = std::make_unique<core::DataVisitor<TIn>>(
        in_channel, kQueueDepth, [stage, box_impl, pub, mailbox] {
          auto* visitor_ptr = *stage;
          if (visitor_ptr == nullptr) {
            return;
          }
          while (TIn* msg = visitor_ptr->try_fetch_0()) {
            pub->parent_ = mailbox->pop();
            box_impl->handle(*msg, *pub);
            pub->parent_ = {};
          }
        });
    *stage = visitor.get();
    stages_.push_back(std::make_unique<detail::VisitorStage<TIn>>(std::move(visitor)));
    init_hooks_.push_back([box_impl, pub] { box_impl->on_init(*pub); });
  }

  // Sink wiring (called by Flow::SinkDecl::wire).
  template <typename T>
  void attach_sink(const std::string& channel,
                   std::function<void(const T&, const core::Lineage&)> fn) {
    const auto mailbox = register_mailbox(channel);
    const auto box = std::make_shared<core::DataVisitor<T>*>(nullptr);
    auto visitor = std::make_unique<core::DataVisitor<T>>(
        channel, kQueueDepth, [box, mailbox, fn = std::move(fn)] {
          auto* visitor_ptr = *box;
          if (visitor_ptr == nullptr) {
            return;
          }
          while (T* msg = visitor_ptr->try_fetch_0()) {
            fn(*msg, mailbox->pop());
          }
        });
    *box = visitor.get();
    stages_.push_back(std::make_unique<detail::VisitorStage<T>>(std::move(visitor)));
  }

  // Interprets the flow: wires maps/joins/sinks, then drives every source
  // on its interval (absolute-deadline pacing, no cumulative drift) until
  // `duration` elapses. Each source runs on its own thread.
  void run_for(const Flow& flow, std::chrono::milliseconds duration);

 private:
  // Creates a mailbox owned by the runtime and registers it for
  // `channel` so publish_bytes fans lineage copies to it.
  std::shared_ptr<detail::LineageMailbox> register_mailbox(const std::string& channel);

  // Publishes with a lineage derived from `parent` (parent chain + this
  // channel's hop with a fresh per-channel seq).
  void publish_derived(const core::Lineage& parent, const std::string& channel, const void* data,
                       std::size_t size);

  // Box publication: `parent` empty (on_init) roots at the channel;
  // otherwise derives from it (handle).
  void publish_box(const std::string& channel, const void* data, std::size_t size,
                   const core::Lineage& parent);

  [[nodiscard]] std::uint64_t next_seq(const std::string& channel);

  static constexpr std::size_t kQueueDepth = 16;

  std::vector<std::unique_ptr<detail::StageHolder>> stages_;
  std::vector<std::shared_ptr<detail::LineageMailbox>> mailboxes_;
  std::vector<std::function<void()>> init_hooks_;

  std::mutex mutex_;
  std::unordered_map<std::string, std::vector<detail::LineageMailbox*>> channel_queues_;
  std::unordered_map<std::string, std::uint64_t> seq_counters_;
};

// Defined after FlowRuntime completes: publish reaches into the runtime
// (two-phase lookup — same pattern as flow.h's detail::make_* helpers).
template <typename T>
void BoxPub<T>::publish(const T& msg) {
  rt_->publish_box(channel_, &msg, sizeof(T), parent_);
}

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

template <typename TA, typename TB, typename TC>
std::function<void(FlowRuntime&)> make_join_wire(std::string in_a, std::string in_b,
                                                 std::string out_channel,
                                                 std::function<TC(const TA&, const TB&)> fn) {
  return [in_a = std::move(in_a), in_b = std::move(in_b), out_channel = std::move(out_channel),
          fn = std::move(fn)](FlowRuntime& rt) {
    rt.template attach_join<TA, TB, TC>(in_a, in_b, out_channel, fn);
  };
}

template <typename TIn, typename TOut, typename TBox>
std::function<void(FlowRuntime&)> make_box_wire(std::string in_channel, std::string out_channel,
                                                TBox box) {
  return [in_channel = std::move(in_channel), out_channel = std::move(out_channel),
          box](FlowRuntime& rt) {
    rt.template attach_box<TIn, TOut, TBox>(in_channel, out_channel, box);
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
