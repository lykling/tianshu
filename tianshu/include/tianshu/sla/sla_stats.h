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

// SLA runtime defense (ADR-0029 D6 / v0.5): per-endpoint end-to-end
// latency histograms + deadline-miss counters. This is drift detection
// for graphs that already passed load-time verification — not SLA
// enforcement. Recording exists only on channels declared as SLA
// endpoints; every other channel pays one null check.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace tianshu::sla {

struct SlaEndpointStats {
  std::string endpoint;
  std::chrono::microseconds deadline{0};
  std::uint64_t count{0};
  std::uint64_t miss_count{0};
  std::chrono::nanoseconds p50{0};  // bucket-approximated (log scale)
  std::chrono::nanoseconds p99{0};
};

// Per-endpoint histogram with lock-free recording: bucket counters are
// atomics, so the source-cascade threads never contend. Endpoints are
// registered during wiring (before any recording); snapshot() is for
// observers (ti monitor, demos) and takes no hot-path locks either.
class SlaStatsCollector {
 public:
  // Wiring-time registration; must complete before record() calls.
  void add_endpoint(const std::string& channel, std::chrono::microseconds deadline);

  // Hot path: no-op when the channel is not a registered endpoint.
  void record_if_endpoint(const std::string& channel, std::chrono::nanoseconds e2e);

  [[nodiscard]] std::vector<SlaEndpointStats> snapshot() const;

 private:
  struct EndpointState {
    std::string channel;
    std::chrono::microseconds deadline;
    std::atomic<std::uint64_t> buckets[9]{};  // 8 edges + overflow
    std::atomic<std::uint64_t> miss_count{0};
    std::atomic<std::uint64_t> total{0};
  };

  // Static so the bucket-edge lookup stays a pure function.
  static int bucket_of(std::chrono::nanoseconds e2e);
  static std::chrono::nanoseconds bucket_floor(int bucket);

  std::unordered_map<std::string, std::shared_ptr<EndpointState>> endpoints_;
};

}  // namespace tianshu::sla
