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

// Phase 0 smoke benchmark: measure version API call overhead.
// This validates GoogleBenchmark integration and sets a baseline
// for future L4-PRIM / L4-TRANS benchmarks.

#include <benchmark/benchmark.h>

#include "tianshu/version.h"

static void BM_VersionMajor(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(tianshu_version_major());
  }
}
BENCHMARK(BM_VersionMajor);

static void BM_VersionString(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(tianshu_version_string());
  }
}
BENCHMARK(BM_VersionString);

static void BM_BuildProfile(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(tianshu_build_profile());
  }
}
BENCHMARK(BM_BuildProfile);

BENCHMARK_MAIN();
