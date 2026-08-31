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

// DSL v0: declarative flow declarations (per ADR-0021).
//
// The builder records a strongly-typed graph; the runtime (dsl_runtime.h)
// interprets it on the L4 stack. This header is declaration-only, so the
// same Flow can later feed the L1 compiler unchanged.

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tianshu/core/lineage.h"
#include "tianshu/core/message_traits.h"

namespace tianshu::dsl {

class FlowRuntime;

namespace detail {
// Defined in dsl_runtime.h AFTER FlowRuntime is complete: the lambda
// bodies perform member access on FlowRuntime, which must stay a
// dependent expression (two-phase lookup would otherwise require a
// complete type at this point).
template <typename T>
std::function<void(FlowRuntime&, std::uint64_t)> make_source_drive(
    std::string channel, std::function<T(std::uint64_t)> emit);

template <typename TIn, typename TOut>
std::function<void(FlowRuntime&)> make_map_wire(std::string in_channel, std::string out_channel,
                                                std::function<TOut(const TIn&)> fn);

template <typename TA, typename TB, typename TC>
std::function<void(FlowRuntime&)> make_join_wire(std::string in_a, std::string in_b,
                                                 std::string out_channel,
                                                 std::function<TC(const TA&, const TB&)> fn);

template <typename T>
std::function<void(FlowRuntime&)> make_sink_wire(
    std::string channel, std::function<void(const T&, const core::Lineage&)> fn);
}  // namespace detail

// Typed handle to a graph edge; the template parameter makes wiring
// mistakes a compile error.
template <typename T>
class Stream {
 public:
  Stream() = default;
  Stream(std::string channel, std::string type_name)
      : channel_(std::move(channel)), type_name_(std::move(type_name)) {}

  [[nodiscard]] const std::string& channel() const { return channel_; }
  [[nodiscard]] const std::string& type_name() const { return type_name_; }
  [[nodiscard]] bool valid() const { return !channel_.empty(); }

 private:
  std::string channel_;
  std::string type_name_;
};

// ---------------------------------------------------------------------------
// The recorded declaration graph (immutable after build())
// ---------------------------------------------------------------------------

class Flow {
 public:
  // Type-erased but self-describing node records: the runtime walks
  // these; the future compiler consumes these.
  struct SourceDecl {
    std::string channel;
    std::chrono::milliseconds interval;
    std::string type_name;
    // One-tick driver: publishes the payload + its root lineage.
    std::function<void(FlowRuntime& rt, std::uint64_t tick)> drive;
  };
  struct MapDecl {
    std::string in_channel;
    std::string out_channel;
    std::string in_type_name;
    std::string out_type_name;
    // Wiring installer: attaches the transformation to the runtime.
    std::function<void(FlowRuntime& rt)> wire;
  };
  struct JoinDecl {
    std::string in_channel_a;
    std::string in_channel_b;
    std::string out_channel;
    std::string type_name_a;
    std::string type_name_b;
    std::string out_type_name;
    std::function<void(FlowRuntime& rt)> wire;
  };
  struct SinkDecl {
    std::string channel;
    std::string type_name;
    std::function<void(FlowRuntime& rt)> wire;
  };

  [[nodiscard]] const std::string& name() const { return name_; }
  [[nodiscard]] const std::vector<SourceDecl>& sources() const { return sources_; }
  [[nodiscard]] const std::vector<MapDecl>& maps() const { return maps_; }
  [[nodiscard]] const std::vector<JoinDecl>& joins() const { return joins_; }
  [[nodiscard]] const std::vector<SinkDecl>& sinks() const { return sinks_; }

  // Wiring summary: "src -> map -> sink" with channels, for tests.
  [[nodiscard]] std::string describe() const {
    std::string out = "flow " + name_ + ":";
    for (const auto& s : sources_) {
      out += " src[" + s.channel + "]";
    }
    for (const auto& m : maps_) {
      out += " map[" + m.in_channel + " -> " + m.out_channel + "]";
    }
    for (const auto& j : joins_) {
      out += " join[" + j.in_channel_a + " + " + j.in_channel_b + " -> " + j.out_channel + "]";
    }
    for (const auto& s : sinks_) {
      out += " sink[" + s.channel + "]";
    }
    return out;
  }

 private:
  friend class FlowBuilder;
  explicit Flow(std::string name) : name_(std::move(name)) {}

  std::string name_;
  std::vector<SourceDecl> sources_;
  std::vector<MapDecl> maps_;
  std::vector<JoinDecl> joins_;
  std::vector<SinkDecl> sinks_;
};

// ---------------------------------------------------------------------------
// Builder + chained API
//
//   auto flow = FlowBuilder("demo")
//                   .source<T>("ticks", 10ms, emit)
//                   .map<T, U>(fn)
//                   .sink<U>(collect)
//                   .build();
//
// FlowChain holds a raw pointer to its builder: the chained expression is
// one full statement, so a temporary builder stays alive through the whole
// chain (v0 constraint; split chains require keeping the builder alive).
// ---------------------------------------------------------------------------

template <typename T>
class FlowChain;

class FlowBuilder {
 public:
  explicit FlowBuilder(std::string name) : name_(std::move(name)) {}

  template <typename T>
  FlowChain<T> source(std::string_view name, std::chrono::milliseconds interval,
                      std::function<T(std::uint64_t)> emit);

  // AllLatest fusion of two streams (ADR-0021 amendment): fires when both
  // inputs are non-empty, consumes one message from each; the output
  // lineage merges both parents' branches.
  template <typename TA, typename TB, typename TC>
  FlowChain<TC> join(const FlowChain<TA>& a, const FlowChain<TB>& b,
                     std::function<TC(const TA&, const TB&)> fn);

  // Accepted and ignored in v0 (SLA annotation slot; L1 compiler consumes
  // it later per ADR-0021).
  FlowBuilder& with_sla(std::string_view sla);

  [[nodiscard]] Flow build();

 private:
  template <typename U>
  friend class FlowChain;

  template <typename TIn, typename TOut>
  Stream<TOut> map_stream(const Stream<TIn>& in, std::function<TOut(const TIn&)> fn);

  template <typename TIn, typename TOut>
  Stream<TOut> map_stream_to(const Stream<TIn>& in, std::string_view out_name,
                             std::function<TOut(const TIn&)> fn);

  template <typename TIn, typename TOut>
  Stream<TOut> emit_map(const Stream<TIn>& in, const std::string& out_channel,
                        std::function<TOut(const TIn&)> fn);

  template <typename T>
  void sink_stream(const Stream<T>& in, std::function<void(const T&, const core::Lineage&)> fn);

  [[nodiscard]] std::string channel_for(std::string_view name) const {
    return name_ + "/" + std::string(name);
  }
  std::string anon_channel() { return name_ + "/~" + std::to_string(anon_++); }

  std::string name_;
  std::vector<Flow::SourceDecl> sources_;
  std::vector<Flow::MapDecl> maps_;
  std::vector<Flow::JoinDecl> joins_;
  std::vector<Flow::SinkDecl> sinks_;
  std::uint64_t anon_{0};
};

template <typename T>
class FlowChain {
 public:
  template <typename TOut>
  FlowChain<TOut> map(std::function<TOut(const T&)> fn) const {
    return FlowChain<TOut>(builder_, builder_->map_stream<T, TOut>(stream_, std::move(fn)));
  }

  // Map with an explicit output channel (feedback edges: a stage writing
  // back into a channel that other stages also publish to or join on).
  template <typename TOut>
  FlowChain<TOut> map_to(std::string_view out_name, std::function<TOut(const T&)> fn) const {
    return FlowChain<TOut>(builder_,
                           builder_->map_stream_to<T, TOut>(stream_, out_name, std::move(fn)));
  }

  FlowChain<T>& with_sla(std::string_view sla) {
    builder_->with_sla(sla);
    return *this;
  }

  FlowChain<T>& sink(std::function<void(const T&, const core::Lineage&)> fn) {
    builder_->sink_stream<T>(stream_, std::move(fn));
    return *this;
  }

  [[nodiscard]] Flow build() { return builder_->build(); }

 private:
  friend class FlowBuilder;
  template <typename U>
  friend class FlowChain;

  FlowChain(FlowBuilder* builder, Stream<T> stream)
      : builder_(builder), stream_(std::move(stream)) {}

  FlowBuilder* builder_;
  Stream<T> stream_;
};

// --- FlowBuilder member definitions (after FlowChain, for name lookup) ---

template <typename T>
FlowChain<T> FlowBuilder::source(std::string_view name, std::chrono::milliseconds interval,
                                 std::function<T(std::uint64_t)> emit) {
  const std::string ch = channel_for(name);
  Flow::SourceDecl decl{ch, interval, std::string(core::MessageTraits<T>::name()),
                        detail::make_source_drive<T>(ch, std::move(emit))};
  sources_.push_back(std::move(decl));
  return FlowChain<T>(this, Stream<T>(ch, std::string(core::MessageTraits<T>::name())));
}

inline FlowBuilder& FlowBuilder::with_sla(std::string_view /*sla*/) { return *this; }

inline Flow FlowBuilder::build() {
  Flow flow(name_);
  flow.sources_ = std::move(sources_);
  flow.maps_ = std::move(maps_);
  flow.joins_ = std::move(joins_);
  flow.sinks_ = std::move(sinks_);
  return flow;
}

template <typename TIn, typename TOut>
Stream<TOut> FlowBuilder::map_stream(const Stream<TIn>& in, std::function<TOut(const TIn&)> fn) {
  const std::string out = anon_channel();
  Flow::MapDecl decl{in.channel(), out, in.type_name(),
                     std::string(core::MessageTraits<TOut>::name()),
                     detail::make_map_wire<TIn, TOut>(in.channel(), out, std::move(fn))};
  maps_.push_back(std::move(decl));
  return Stream<TOut>(out, std::string(core::MessageTraits<TOut>::name()));
}

template <typename TIn, typename TOut>
Stream<TOut> FlowBuilder::map_stream_to(const Stream<TIn>& in, std::string_view out_name,
                                        std::function<TOut(const TIn&)> fn) {
  const std::string out = channel_for(out_name);
  Flow::MapDecl decl{in.channel(), out, in.type_name(),
                     std::string(core::MessageTraits<TOut>::name()),
                     detail::make_map_wire<TIn, TOut>(in.channel(), out, std::move(fn))};
  maps_.push_back(std::move(decl));
  return Stream<TOut>(out, std::string(core::MessageTraits<TOut>::name()));
}

template <typename T>
void FlowBuilder::sink_stream(const Stream<T>& in,
                              std::function<void(const T&, const core::Lineage&)> fn) {
  Flow::SinkDecl decl{in.channel(), in.type_name(),
                      detail::make_sink_wire<T>(in.channel(), std::move(fn))};
  sinks_.push_back(std::move(decl));
}

template <typename TA, typename TB, typename TC>
FlowChain<TC> FlowBuilder::join(const FlowChain<TA>& a, const FlowChain<TB>& b,
                                std::function<TC(const TA&, const TB&)> fn) {
  const std::string out = anon_channel();
  Flow::JoinDecl decl{a.stream_.channel(),
                      b.stream_.channel(),
                      out,
                      a.stream_.type_name(),
                      b.stream_.type_name(),
                      std::string(core::MessageTraits<TC>::name()),
                      detail::make_join_wire<TA, TB, TC>(a.stream_.channel(), b.stream_.channel(),
                                                         out, std::move(fn))};
  joins_.push_back(std::move(decl));
  return FlowChain<TC>(this, Stream<TC>(out, std::string(core::MessageTraits<TC>::name())));
}

}  // namespace tianshu::dsl
