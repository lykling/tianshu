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
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "tianshu/core/component.h"
#include "tianshu/core/data_visitor.h"
#include "tianshu/core/lineage.h"
#include "tianshu/core/node.h"
#include "tianshu/dsl/flow.h"
#include "tianshu/transport/transport_backend.h"

namespace tianshu::dsl {

// Op publish handle (ADR-0024): bound to one output channel of the op.
// Valid ONLY inside on_init/handle invocations; publish derives lineage
// from the current input when called from handle (map semantics) and
// roots at the box channel when called from on_init (source semantics).
template <typename T>
class OpPub {
 public:
  void publish(const T& msg);

 private:
  friend class FlowRuntime;

  OpPub(FlowRuntime* rt, std::string channel) : rt_(rt), channel_(std::move(channel)) {}

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

// One retained entry of a channel's bounded history (ADR-0026/0027):
// the bytes, the channel-local seq, and the message's lineage.
struct HistoryEntry {
  std::uint64_t seq{0};
  std::vector<std::uint8_t> bytes;
  core::Lineage lineage;
};

// Bounded per-channel history ring: slice queries and state recovery
// read from here; publish_bytes captures every message.
class HistoryRing {
 public:
  explicit HistoryRing(std::size_t depth) : depth_(depth) {}

  void push(std::uint64_t seq, const void* data, std::size_t size, const core::Lineage& lin) {
    const auto* b = static_cast<const std::uint8_t*>(data);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    std::vector<std::uint8_t> bytes(b, b + size);
    entries_.push_back(HistoryEntry{.seq = seq, .bytes = std::move(bytes), .lineage = lin});
    if (entries_.size() > depth_) {
      entries_.pop_front();
    }
  }

  [[nodiscard]] const std::deque<HistoryEntry>& entries() const { return entries_; }

 private:
  std::size_t depth_;
  std::deque<HistoryEntry> entries_;
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
  mutable std::mutex mutex_;
  std::deque<core::Lineage> queue_;
};

}  // namespace detail

class FlowRuntime {
 public:
  FlowRuntime() = default;
  ~FlowRuntime();

  FlowRuntime(const FlowRuntime&) = delete;
  FlowRuntime& operator=(const FlowRuntime&) = delete;

  template <typename U>
  friend class OpPub;

  // Copies `lineage` to every consumer mailbox on `channel`, captures
  // the message into the channel's bounded history, then cascades the
  // payload through the DataDispatcher (synchronous chain).
  void publish_bytes(const std::string& channel, const void* data, std::size_t size,
                     const core::Lineage& lineage);

  // Bounded history of a published channel (nullptr when never
  // published): (seq, bytes, lineage) entries, oldest first. Recovery
  // and slice queries read from here (ADR-0026/0027).
  [[nodiscard]] const detail::HistoryRing* history(const std::string& channel) const;

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

  // Op wiring (called by Flow::OpDecl::wire): visitor on the input
  // channel + deferred on_init (runs after ALL wiring so consumers of the
  // output channel are registered before the bootstrap publication).
  template <typename TIn, typename TOut, typename TOp>
  void attach_op(const std::string& in_channel, const std::string& out_channel, TOp impl) {
    const std::shared_ptr<OpPub<TOut>> pub(new OpPub<TOut>(this, out_channel));
    const auto mailbox = register_mailbox(in_channel);
    const auto op_impl = std::make_shared<TOp>(std::move(impl));
    const auto stage = std::make_shared<core::DataVisitor<TIn>*>(nullptr);
    auto visitor = std::make_unique<core::DataVisitor<TIn>>(
        in_channel, kQueueDepth, [stage, op_impl, pub, mailbox] {
          auto* visitor_ptr = *stage;
          if (visitor_ptr == nullptr) {
            return;
          }
          while (TIn* msg = visitor_ptr->try_fetch_0()) {
            pub->parent_ = mailbox->pop();
            op_impl->handle(*msg, *pub);
            pub->parent_ = {};
          }
        });
    *stage = visitor.get();
    stages_.push_back(std::make_unique<detail::VisitorStage<TIn>>(std::move(visitor)));
    init_hooks_.push_back([op_impl, pub] { op_impl->on_init(*pub); });
  }

  // Referenced-component wiring (ADR-0025, called by Flow::FromDecl::wire).
  // Source-like: timer-driven publisher; the component keeps its own
  // thread (L4 behavior). Component-like: proc runs on the publisher's
  // dispatch thread via its DataVisitor. Both pump outputs back through
  // an intra reader -> publish_bytes with rooted lineage.
  void attach_referenced_source(const std::string& registry_name, const std::string& out_channel,
                                std::chrono::milliseconds interval);
  void attach_referenced_component(const std::string& registry_name, const std::string& in_channel,
                                   const std::string& out_channel);

  // Span wiring (called by Flow::SpanDecl::wire, ADR-0026): visitor on
  // the TRIGGER channel; on each trigger, materialize the data channel's
  // history slice where time(msg) in [t0,t1] and publish with merged
  // lineage (trigger branch + data RANGE branch).
  template <typename TTrig, typename TData, typename TOut, typename TSpanFn, typename TTimeFn,
            typename TImpl>
  void attach_span(const std::string& trig_channel, const std::string& data_channel,
                   const std::string& out_channel, TSpanFn span_fn, TTimeFn time_fn, TImpl impl) {
    const auto mailbox = register_mailbox(trig_channel);
    const auto op_impl = std::make_shared<TImpl>(std::move(impl));
    const auto span = std::make_shared<TSpanFn>(std::move(span_fn));
    const auto time_of = std::make_shared<TTimeFn>(std::move(time_fn));
    const auto stage = std::make_shared<core::DataVisitor<TTrig>*>(nullptr);
    auto visitor = std::make_unique<core::DataVisitor<TTrig>>(
        trig_channel, kQueueDepth,
        [this, stage, mailbox, op_impl, span, time_of, data_channel, out_channel] {
          auto* visitor_ptr = *stage;
          if (visitor_ptr == nullptr) {
            return;
          }
          while (TTrig* trig = visitor_ptr->try_fetch_0()) {
            const core::Lineage parent = mailbox->pop();
            const auto range = (*span)(*trig);

            Slice<TData> slice;
            slice.seq_lo = 1;  // empty marker: lo > hi
            const auto* hist = history(data_channel);
            if (hist != nullptr) {
              for (const auto& entry : hist->entries()) {
                TData msg{};
                if (entry.bytes.size() != sizeof(TData)) {
                  continue;
                }
                std::memcpy(&msg, entry.bytes.data(), sizeof(msg));
                const std::uint64_t t = (*time_of)(msg);
                if (t < range.first || t > range.second) {
                  continue;
                }
                if (slice.items.empty()) {
                  slice.seq_lo = entry.seq;
                }
                slice.seq_hi = entry.seq;
                slice.items.push_back(msg);
              }
              const auto& entries = hist->entries();
              if (!entries.empty() && entries.front().seq > 0) {
                TData front_msg{};
                std::memcpy(&front_msg, entries.front().bytes.data(), sizeof(front_msg));
                slice.truncated = (*time_of)(front_msg) > range.first;
              }
            }

            TOut out = (*op_impl)(*trig, slice);
            core::Lineage lin = parent;
            if (!slice.empty()) {
              lin.merge(core::Lineage::rooted_range(data_channel, slice.seq_lo, slice.seq_hi));
            }
            publish_derived(lin, out_channel, &out, sizeof(TOut));
          }
        });
    *stage = visitor.get();
    stages_.push_back(std::make_unique<detail::VisitorStage<TTrig>>(std::move(visitor)));
  }

  // Stateful wiring (called by Flow::StatefulDecl::wire, ADR-0027):
  // one input visitor, TWO publish handles (output + state); both share
  // the input lineage as parent, so a state version's provenance points
  // at exactly the input that produced it — the recovery protocol reads
  // that to find the absorption point.
  template <typename TIn, typename TOut, typename TState, typename TImpl>
  void attach_stateful(const std::string& in_channel, const std::string& out_channel,
                       const std::string& state_channel, TImpl impl) {
    const std::shared_ptr<OpPub<TOut>> out_pub(new OpPub<TOut>(this, out_channel));
    const std::shared_ptr<OpPub<TState>> state_pub(new OpPub<TState>(this, state_channel));
    const auto mailbox = register_mailbox(in_channel);
    const auto op_impl = std::make_shared<TImpl>(std::move(impl));
    const auto stage = std::make_shared<core::DataVisitor<TIn>*>(nullptr);
    auto visitor = std::make_unique<core::DataVisitor<TIn>>(
        in_channel, kQueueDepth, [stage, op_impl, out_pub, state_pub, mailbox] {
          auto* visitor_ptr = *stage;
          if (visitor_ptr == nullptr) {
            return;
          }
          while (TIn* msg = visitor_ptr->try_fetch_0()) {
            const core::Lineage parent = mailbox->pop();
            out_pub->parent_ = parent;
            state_pub->parent_ = parent;
            op_impl->handle(*msg, *out_pub, *state_pub);
            out_pub->parent_ = {};
            state_pub->parent_ = {};
          }
        });
    *stage = visitor.get();
    stages_.push_back(std::make_unique<detail::VisitorStage<TIn>>(std::move(visitor)));
    init_hooks_.emplace_back(
        [op_impl, out_pub, state_pub] { op_impl->on_init(*out_pub, *state_pub); });
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

  // Op publication: `parent` empty (on_init) roots at the channel;
  // otherwise derives from it (handle).
  void publish_op(const std::string& channel, const void* data, std::size_t size,
                  const core::Lineage& parent);

  // Pumps a referenced component's transport output back into the DSL
  // channel world with rooted lineage (ADR-0025 Q3).
  void attach_bridge_reader(const std::string& out_channel);

  [[nodiscard]] std::uint64_t next_seq(const std::string& channel);

  static constexpr std::size_t kQueueDepth = 16;
  static constexpr std::size_t kHistoryDepth = 64;

  std::vector<std::unique_ptr<detail::StageHolder>> stages_;
  std::vector<std::shared_ptr<detail::LineageMailbox>> mailboxes_;
  std::vector<std::function<void()>> init_hooks_;

  // Referenced-component plumbing (ADR-0025). Declaration order matters:
  // components must be destroyed before the node and bridge readers.
  std::unique_ptr<core::Node> bridge_node_;
  std::vector<std::unique_ptr<transport::ReaderBase>> bridge_readers_;
  std::vector<std::shared_ptr<core::ComponentBase>> components_;

  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::vector<detail::LineageMailbox*>> channel_queues_;
  std::unordered_map<std::string, std::uint64_t> seq_counters_;
  std::unordered_map<std::string, detail::HistoryRing> histories_;
};

// Defined after FlowRuntime completes: publish reaches into the runtime
// (two-phase lookup — same pattern as flow.h's detail::make_* helpers).
template <typename T>
void OpPub<T>::publish(const T& msg) {
  rt_->publish_op(channel_, &msg, sizeof(T), parent_);
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

template <typename TIn, typename TOut, typename TOp>
std::function<void(FlowRuntime&)> make_op_wire(std::string in_channel, std::string out_channel,
                                               TOp impl) {
  return [in_channel = std::move(in_channel), out_channel = std::move(out_channel),
          impl](FlowRuntime& rt) {
    rt.template attach_op<TIn, TOut, TOp>(in_channel, out_channel, impl);
  };
}

template <typename TTrig, typename TData, typename TOut, typename TSpanFn, typename TTimeFn,
          typename TImpl>
std::function<void(FlowRuntime&)> make_span_wire(std::string trig_channel, std::string data_channel,
                                                 std::string out_channel, TSpanFn span_fn,
                                                 TTimeFn time_fn, TImpl impl) {
  return [trig_channel = std::move(trig_channel), data_channel = std::move(data_channel),
          out_channel = std::move(out_channel), span_fn = std::move(span_fn),
          time_fn = std::move(time_fn), impl = std::move(impl)](FlowRuntime& rt) mutable {
    rt.template attach_span<TTrig, TData, TOut, TSpanFn, TTimeFn, TImpl>(
        trig_channel, data_channel, out_channel, std::move(span_fn), std::move(time_fn),
        std::move(impl));
  };
}

template <typename TIn, typename TOut, typename TState, typename TImpl>
std::function<void(FlowRuntime&)> make_stateful_wire(std::string in_channel,
                                                     std::string out_channel,
                                                     std::string state_channel, TImpl impl) {
  return
      [in_channel = std::move(in_channel), out_channel = std::move(out_channel),
       state_channel = std::move(state_channel), impl = std::move(impl)](FlowRuntime& rt) mutable {
        rt.template attach_stateful<TIn, TOut, TState, TImpl>(in_channel, out_channel,
                                                              state_channel, std::move(impl));
      };
}

template <typename TOut>
bool probe_source_shape(std::string_view registry_name) {
  const auto comp =
      core::ComponentFactory::instance().create(registry_name, std::string(registry_name));
  return comp != nullptr && dynamic_cast<core::TimerSourceComponent<TOut>*>(comp.get()) != nullptr;
}

template <typename TIn, typename TOut>
bool probe_component_shape(std::string_view registry_name) {
  const auto comp =
      core::ComponentFactory::instance().create(registry_name, std::string(registry_name));
  return comp != nullptr && dynamic_cast<core::Component<TIn, TOut>*>(comp.get()) != nullptr;
}

template <typename TOut>
std::function<void(FlowRuntime&)> make_from_source_wire(std::string registry_name,
                                                        std::string out_channel,
                                                        std::chrono::milliseconds interval) {
  return [registry_name = std::move(registry_name), out_channel = std::move(out_channel),
          interval](FlowRuntime& rt) {
    rt.attach_referenced_source(registry_name, out_channel, interval);
  };
}

template <typename TIn, typename TOut>
std::function<void(FlowRuntime&)> make_from_component_wire(std::string registry_name,
                                                           std::string in_channel,
                                                           std::string out_channel) {
  return [registry_name = std::move(registry_name), in_channel = std::move(in_channel),
          out_channel = std::move(out_channel)](FlowRuntime& rt) {
    rt.attach_referenced_component(registry_name, in_channel, out_channel);
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
