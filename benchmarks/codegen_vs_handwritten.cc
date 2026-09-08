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

// H2 verdict rig (ADR-0030 D5, roadmap 1.3): the same chain shape run
// three ways, one million messages each —
//   handwritten : CacheBuffer + DataDispatcher wired by hand, no
//                 FlowRuntime machinery (the gold standard)
//   interpreted : FlowBuilder -> FlowRuntime wiring (publish_bytes)
//   compiled    : the pipeline's .so artifact installing its wiring
// Per-message e2e is stamped into the payload at the source and
// measured at the terminal sink; P50/P99/P999 reported as counters.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "tianshu/base/cache_buffer.h"
#include "tianshu/compiler/pipeline.h"
#include "tianshu/core/data_dispatcher.h"
#include "tianshu/core/data_visitor.h"
#include "tianshu/core/lineage.h"
#include "tianshu/core/message_traits.h"
#include "tianshu/dsl/dsl_runtime.h"
#include "tianshu/dsl/flow.h"

namespace {

using tianshu::base::CacheBuffer;
using tianshu::compiler::Pipeline;
using tianshu::core::DataDispatcher;
using tianshu::core::Lineage;

struct BenchMsg {
  std::uint64_t born_ns{0};
  std::uint64_t seq{0};
};

}  // namespace

TIANSHU_TRAITS_POD(BenchMsg, "bench.BenchMsg");

namespace {

constexpr int kMessages = 1'000'000;

[[nodiscard]] std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}

BenchMsg transform(const BenchMsg& in) { return BenchMsg{.born_ns = in.born_ns, .seq = in.seq + 1}; }

void report_percentiles(benchmark::State& state, const std::vector<std::uint64_t>& latencies,
                        int hops) {
  std::vector<std::uint64_t> sorted(latencies);
  std::ranges::sort(sorted);
  const auto pick = [&sorted](double f) {
    const auto rank = static_cast<std::size_t>(f * static_cast<double>(sorted.size()));
    return sorted[std::min(sorted.size() - 1, rank)];
  };
  state.counters["hops"] = hops;
  state.counters["p50_ns"] = static_cast<double>(pick(0.50));
  state.counters["p99_ns"] = static_cast<double>(pick(0.99));
  state.counters["p999_ns"] = static_cast<double>(pick(0.999));
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(sorted.size()));
}

// ---------------------------------------------------------------------------
// Handwritten: CacheBuffer + dispatcher per hop, wired by hand.
// ---------------------------------------------------------------------------

struct HandHop {
  CacheBuffer<BenchMsg> buf{16};
  std::function<void()> notify;
};

class HandChain {
 public:
  HandChain(const std::string& prefix, int hops,
            std::vector<std::uint64_t>* sink_latencies)
      : sink_latencies_(*sink_latencies), owner_(this) {
    auto& dispatcher = DataDispatcher::instance();
    const auto hop_count = static_cast<std::size_t>(hops);
    for (std::size_t i = 0; i < hop_count; ++i) {
      channels_.push_back(prefix + "/hop" + std::to_string(i));
    }
    channels_.push_back(prefix + "/out");
    for (std::size_t i = 0; i < hop_count; ++i) {
      hops_.push_back(std::make_unique<HandHop>());
    }
    sink_ = std::make_unique<HandHop>();

    for (std::size_t i = 0; i < hop_count; ++i) {
      const std::uint64_t next = tianshu::core::channel_id_for(channels_[i + 1]);
      hops_[i]->notify = [this, i, next] {
        while (BenchMsg* msg = hops_[i]->buf.try_fetch()) {
          const BenchMsg out = transform(*msg);
          DataDispatcher::instance().dispatch(next, &out, sizeof(out));
        }
      };
      dispatcher.add_buffer(tianshu::core::channel_id_for(channels_[i]), &hops_[i]->buf,
                            hops_[i]->notify, owner_);
    }
    sink_->notify = [this] {
      while (BenchMsg* msg = sink_->buf.try_fetch()) {
        sink_latencies_.push_back(now_ns() - msg->born_ns);
      }
    };
    dispatcher.add_buffer(tianshu::core::channel_id_for(channels_.back()), &sink_->buf,
                          sink_->notify, owner_);
  }

  ~HandChain() { DataDispatcher::instance().remove_owner(owner_); }

  void drive(std::size_t messages) {
    const std::uint64_t first = tianshu::core::channel_id_for(channels_.front());
    for (std::size_t i = 0; i < messages; ++i) {
      const BenchMsg msg{.born_ns = now_ns(), .seq = static_cast<std::uint64_t>(i)};
      DataDispatcher::instance().dispatch(first, &msg, sizeof(msg));
    }
  }

 private:
  std::vector<std::string> channels_;
  std::vector<std::unique_ptr<HandHop>> hops_;
  std::unique_ptr<HandHop> sink_;
  std::vector<std::uint64_t>& sink_latencies_;
  const void* owner_;
};

// ---------------------------------------------------------------------------
// DSL flow with the same shape; interpreted and compiled share it.
// ---------------------------------------------------------------------------

tianshu::dsl::Flow make_flow(const std::string& name, int hops,
                             std::vector<std::uint64_t>* sink_latencies) {
  tianshu::dsl::FlowBuilder b(name);
  auto chain = b.source<BenchMsg>("src", std::chrono::milliseconds(1), [](std::uint64_t t) {
    return BenchMsg{.born_ns = 0, .seq = t};
  });
  for (int i = 0; i < hops; ++i) {
    chain = chain.map<BenchMsg>([](const BenchMsg& in) { return transform(in); });
  }
  static_cast<void>(hops);
  chain.sink([sink_latencies](const BenchMsg& msg, const Lineage&) {
    sink_latencies->push_back(now_ns() - msg.born_ns);
  });
  return b.build();
}

void drive_runtime(tianshu::dsl::FlowRuntime& rt, const tianshu::dsl::Flow& flow, int messages) {
  const std::string& channel = flow.sources().front().channel;
  rt.wire(flow);
  for (int i = 0; i < messages; ++i) {
    const BenchMsg msg{.born_ns = now_ns(), .seq = static_cast<std::uint64_t>(i)};
    rt.publish_bytes(channel, &msg, sizeof(msg),
                      Lineage::rooted(channel, static_cast<std::uint64_t>(i)));
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Benchmarks: shape x implementation.
// ---------------------------------------------------------------------------

void run_handwritten(benchmark::State& state, int hops) {
  for (auto _ : state) {
    std::vector<std::uint64_t> latencies;
    latencies.reserve(kMessages);
    HandChain chain("hw" + std::to_string(hops), hops, &latencies);
    chain.drive(kMessages);
    report_percentiles(state, latencies, hops);
  }
}

void run_interpreted(benchmark::State& state, int hops) {
  for (auto _ : state) {
    std::vector<std::uint64_t> latencies;
    latencies.reserve(kMessages);
    auto flow = make_flow("itp" + std::to_string(hops), hops, &latencies);
    tianshu::dsl::FlowRuntime rt;
    drive_runtime(rt, flow, kMessages);
    report_percentiles(state, latencies, hops);
  }
}

void run_compiled(benchmark::State& state, int hops) {
  for (auto _ : state) {
    std::vector<std::uint64_t> latencies;
    latencies.reserve(kMessages);
    auto flow = make_flow("cmp" + std::to_string(hops), hops, &latencies);
    tianshu::compiler::CompileOptions opts;
    opts.cache_dir = "/tmp/tianshu-h2-cache";
    auto compiled = tianshu::compiler::Pipeline::compile(flow, opts);
    tianshu::dsl::FlowRuntime rt;
    // 0ms: install the artifact's wiring without driving the timer
    // source (its emit stamps born_ns=0, which would poison percentiles).
    compiled.run(rt, flow, std::chrono::milliseconds(0));
    const std::string& channel = flow.sources().front().channel;
    for (int i = 0; i < kMessages; ++i) {
      const BenchMsg msg{.born_ns = now_ns(), .seq = static_cast<std::uint64_t>(i)};
      rt.publish_bytes(channel, &msg, sizeof(msg),
                      Lineage::rooted(channel, static_cast<std::uint64_t>(i)));
    }
    report_percentiles(state, latencies, hops);
  }
}

// Roadmap 1.3 shapes: short (2 ops = 1 map + sink), medium (5), long (10).
BENCHMARK_CAPTURE(run_handwritten, short, 1);
BENCHMARK_CAPTURE(run_interpreted, short, 1);
BENCHMARK_CAPTURE(run_compiled, short, 1);
BENCHMARK_CAPTURE(run_handwritten, medium, 4);
BENCHMARK_CAPTURE(run_interpreted, medium, 4);
BENCHMARK_CAPTURE(run_compiled, medium, 4);
BENCHMARK_CAPTURE(run_handwritten, long, 9);
BENCHMARK_CAPTURE(run_interpreted, long, 9);
BENCHMARK_CAPTURE(run_compiled, long, 9);

BENCHMARK_MAIN();
