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

// Benchmark for ObjectPool<T> (L4-PRIM-1).
// Measures acquire + release overhead in single-threaded and multi-threaded.

#include <benchmark/benchmark.h>

#include "tianshu/base/object_pool.h"

namespace {

struct BenchmarkData {
  double values[8];
  int32_t seq;
};

static void BM_ObjectPoolAcquireRelease(benchmark::State& state) {
  tianshu::base::ObjectPool<BenchmarkData> pool(1024);
  for (auto _ : state) {
    auto* p = pool.acquire();
    benchmark::DoNotOptimize(p);
    pool.release(p);
  }
}
BENCHMARK(BM_ObjectPoolAcquireRelease);

static void BM_ObjectPoolPooledPtr(benchmark::State& state) {
  tianshu::base::ObjectPool<BenchmarkData> pool(1024);
  for (auto _ : state) {
    auto ptr = tianshu::base::make_pooled(pool);
    benchmark::DoNotOptimize(ptr);
    // auto-release on scope exit
  }
}
BENCHMARK(BM_ObjectPoolPooledPtr);

static void BM_RawNewDelete(benchmark::State& state) {
  for (auto _ : state) {
    auto* p = new BenchmarkData();
    benchmark::DoNotOptimize(p);
    delete p;
  }
}
BENCHMARK(BM_RawNewDelete);

}  // namespace

BENCHMARK_MAIN();
