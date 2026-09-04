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

#include "tianshu/sla/sla_stats.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tianshu::sla {
namespace {

// Log-scale bucket edges in nanoseconds (ADR-0029 D6: 8 buckets):
//   [0, 100us) [100us, 1ms) [1ms, 10ms) [10ms, 100ms)
//   [100ms, 1s) [1s, 10s) [10s, 100s) [100s, inf)
constexpr std::int64_t kEdges[8] = {
    100'000,       1'000'000,        10'000'000,        100'000'000,
    1'000'000'000, 10'000'000'000LL, 100'000'000'000LL, 0,
};
constexpr int kBucketCount = 8;
constexpr int kOverflowBucket = kBucketCount;  // >= 100s, index 8

}  // namespace

void SlaStatsCollector::add_endpoint(const std::string& channel,
                                     std::chrono::microseconds deadline) {
  endpoints_[channel] = std::make_shared<EndpointState>();
  auto& state = *endpoints_[channel];
  state.channel = channel;
  state.deadline = deadline;
}

void SlaStatsCollector::record_if_endpoint(const std::string& channel,
                                           std::chrono::nanoseconds e2e) {
  const auto it = endpoints_.find(channel);
  if (it == endpoints_.end()) {
    return;
  }
  EndpointState& state = *it->second;
  state.buckets[bucket_of(e2e)].fetch_add(1, std::memory_order_relaxed);
  state.total.fetch_add(1, std::memory_order_relaxed);
  if (e2e > state.deadline) {
    state.miss_count.fetch_add(1, std::memory_order_relaxed);
  }
}

std::vector<SlaEndpointStats> SlaStatsCollector::snapshot() const {
  std::vector<SlaEndpointStats> out;
  out.reserve(endpoints_.size());
  for (const auto& [channel, state_ptr] : endpoints_) {
    const EndpointState& state = *state_ptr;
    SlaEndpointStats stats;
    stats.endpoint = state.channel;
    stats.deadline = state.deadline;
    stats.count = state.total.load(std::memory_order_relaxed);
    stats.miss_count = state.miss_count.load(std::memory_order_relaxed);

    // Percentiles from cumulative bucket counts; the bucket floor is
    // the conservative estimate (actual value lies within the bucket).
    const auto percentile = [&state](double fraction) {
      const std::uint64_t total = state.total.load(std::memory_order_relaxed);
      if (total == 0) {
        return std::chrono::nanoseconds{0};
      }
      const auto target =
          static_cast<std::uint64_t>(std::lround(static_cast<double>(total) * fraction));
      std::uint64_t cumulative = 0;
      for (int b = 0; b <= kOverflowBucket; ++b) {
        cumulative += state.buckets[b].load(std::memory_order_relaxed);
        if (cumulative >= target && cumulative > 0) {
          return bucket_floor(b);
        }
      }
      return bucket_floor(kOverflowBucket);
    };
    stats.p50 = percentile(0.50);
    stats.p99 = percentile(0.99);

    out.push_back(std::move(stats));
  }
  return out;
}

int SlaStatsCollector::bucket_of(std::chrono::nanoseconds e2e) {
  const std::int64_t ns = e2e.count();
  for (int b = 0; b < kBucketCount; ++b) {
    if (kEdges[b] == 0 || ns < kEdges[b]) {
      return b;
    }
  }
  return kOverflowBucket;
}

std::chrono::nanoseconds SlaStatsCollector::bucket_floor(int bucket) {
  if (bucket <= 0) {
    return std::chrono::nanoseconds{0};
  }
  if (bucket > kBucketCount) {
    return std::chrono::nanoseconds{kEdges[kBucketCount - 1]};
  }
  return std::chrono::nanoseconds{kEdges[bucket - 1]};
}

}  // namespace tianshu::sla
