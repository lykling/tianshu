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

// Lineage recording cost (ADR-0022): the per-message overhead the
// framework adds on every publish. This is the number behind the
// "zero-copy lineage" claim and the H3 calibration baseline.

#include <cstdint>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "tianshu/core/lineage.h"

using tianshu::core::Lineage;
using tianshu::core::LineageHop;

// Source-side: creating the root lineage for one published message.
static void BM_LineageRooted(benchmark::State& state) {
  std::uint64_t seq = 0;
  for (auto _ : state) {
    ++seq;
    benchmark::DoNotOptimize(Lineage::rooted("bench/src", seq));
  }
}
BENCHMARK(BM_LineageRooted);

// Map-stage: one add_hop per message on a chain with the branch count a
// linear map chain actually has (1 branch; hop vector reserved upstream
// by the runtime in the steady state).
static void BM_LineageMapHop(benchmark::State& state) {
  const auto hops = static_cast<std::size_t>(state.range(0));
  for (auto _ : state) {
    Lineage lin = Lineage::rooted("bench/src", 7);
    for (std::size_t i = 0; i < hops; ++i) {
      lin.add_hop(LineageHop{.channel = "bench/hop", .seq = i, .seq_end = i});
    }
    benchmark::DoNotOptimize(lin);
  }
}
BENCHMARK(BM_LineageMapHop)->Arg(1)->Arg(4)->Arg(8);

// Move cost: what a stage pays to carry the parent lineage forward.
static void BM_LineageMove(benchmark::State& state) {
  Lineage src = Lineage::rooted("bench/src", 7);
  for (auto _ : state) {
    Lineage moved = std::move(src);
    src = std::move(moved);
    benchmark::DoNotOptimize(src);
  }
}
BENCHMARK(BM_LineageMove);

// Copy cost: join-side merge needs copies of the incoming branches.
static void BM_LineageCopy(benchmark::State& state) {
  Lineage lin = Lineage::rooted("bench/src", 7);
  for (std::size_t i = 0; i < 4; ++i) {
    lin.add_hop(LineageHop{.channel = "bench/hop", .seq = i, .seq_end = i});
  }
  for (auto _ : state) {
    Lineage copy = lin;
    benchmark::DoNotOptimize(copy);
  }
}
BENCHMARK(BM_LineageCopy);

// describe(): monitoring / record path, not the hot publish path.
static void BM_LineageDescribe(benchmark::State& state) {
  Lineage lin = Lineage::rooted("bench/src", 7);
  for (std::size_t i = 0; i < 4; ++i) {
    lin.add_hop(LineageHop{.channel = "bench/hop", .seq = i, .seq_end = i});
  }
  for (auto _ : state) {
    benchmark::DoNotOptimize(lin.describe());
  }
}
BENCHMARK(BM_LineageDescribe);
BENCHMARK_MAIN();
