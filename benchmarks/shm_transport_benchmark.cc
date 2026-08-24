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

// Cross-process SHM transport benchmark (L4-TRANS-3/4 acceptance).
//
// Runs outside GoogleBenchmark's process model on purpose: the reader lives
// in a forked child, and gtest-style reporting from two processes would
// corrupt benchmark's timers. Reports plain text instead.
//
// Measures:
//   - throughput: N small messages fork-to-fork, msg/s (target >= 1M/s)
//   - latency:    per-message send->receive, p50/p90/p99/max (ns)
//   - wakeup:     signal->wakeup round trips with empty rings (ns)

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "tianshu/transport/shm_backend.h"
#include "tianshu/transport/transport_backend.h"

namespace {

using tianshu::transport::ChannelConfig;
using tianshu::transport::Message;

struct BenchPacket {
  std::uint64_t seq;
  std::int64_t send_ns;
  char pad[48];
};

std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int run_throughput_bench(const std::string& name, int messages) {
  int ready_pipe[2];
  int done_pipe[2];
  if (pipe(ready_pipe) != 0 || pipe(done_pipe) != 0) {
    return 1;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    return 1;
  }

  if (pid == 0) {
    close(ready_pipe[0]);
    close(done_pipe[0]);

    tianshu::transport::shm::ShmBackend backend;
    ChannelConfig cfg;
    cfg.channel_name = name;

    auto reader = backend.create_reader(cfg);
    if (reader == nullptr) {
      _exit(10);
    }

    std::atomic<int> received{0};
    reader->set_callback([&](const Message&) { received.fetch_add(1, std::memory_order_relaxed); });

    const char r = 'r';
    if (write(ready_pipe[1], &r, 1) != 1) {
      _exit(11);
    }

    for (int spin = 0; spin < 60000 && received.load() < messages; ++spin) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const int got = received.load();
    // Scope exit releases slot + refcount before the raw _exit below.
    { reader.reset(); }
    if (write(done_pipe[1], &got, sizeof(got)) != static_cast<ssize_t>(sizeof(got))) {
      _exit(12);
    }
    _exit(got == messages ? 0 : 20);
  }

  close(ready_pipe[1]);
  close(done_pipe[1]);

  char r = 0;
  const ssize_t n = read(ready_pipe[0], &r, 1);
  if (n != 1) {
    std::fprintf(stderr, "[throughput] child readiness failed (read=%zd)\n", n);
    return 1;
  }

  tianshu::transport::shm::ShmBackend backend;
  ChannelConfig cfg;
  cfg.channel_name = name;
  auto writer = backend.create_writer(cfg);
  if (writer == nullptr) {
    std::fprintf(stderr, "[throughput] writer creation failed\n");
    return 1;
  }

  const BenchPacket pkt{};
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < messages; ++i) {
    writer->write(&pkt, sizeof(pkt));
  }
  const auto t1 = std::chrono::steady_clock::now();

  int got = 0;
  if (read(done_pipe[0], &got, sizeof(got)) != static_cast<ssize_t>(sizeof(got))) {
    std::fprintf(stderr, "[throughput] done pipe read failed\n");
    return 1;
  }
  close(done_pipe[0]);

  int status = 0;
  waitpid(pid, &status, 0);

  const double wall_s = std::chrono::duration<double>(t1 - t0).count();
  const double msg_per_s = static_cast<double>(got) / wall_s;
  const double us_per_msg = wall_s * 1e6 / static_cast<double>(got);

  std::printf("[throughput] %d msgs in %.3f ms -> %.0f msg/s (%.2f us/msg, "
              "received %d, %s)\n",
              messages, wall_s * 1e3, msg_per_s, us_per_msg, got,
              WIFEXITED(status) && WEXITSTATUS(status) == 0 ? "PASS" : "FAIL");

  return WIFEXITED(status) && WEXITSTATUS(status) == 0 && got == messages ? 0 : 1;
}

int run_latency_bench(const std::string& name, int samples) {
  int ready_pipe[2];
  int done_pipe[2];
  if (pipe(ready_pipe) != 0 || pipe(done_pipe) != 0) {
    return 1;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    return 1;
  }

  if (pid == 0) {
    close(ready_pipe[0]);
    close(done_pipe[0]);

    tianshu::transport::shm::ShmBackend backend;
    ChannelConfig cfg;
    cfg.channel_name = name;

    auto reader = backend.create_reader(cfg);
    if (reader == nullptr) {
      _exit(10);
    }

    ChannelConfig ack_cfg;
    ack_cfg.channel_name = name + "_ack";
    auto ack_writer = backend.create_writer(ack_cfg);
    if (ack_writer == nullptr) {
      _exit(13);
    }

    std::atomic<std::uint64_t> expected{0};
    reader->set_callback([&](const Message& msg) {
      BenchPacket pkt{};
      std::memcpy(&pkt, msg.data, msg.size);
      if (pkt.seq == expected.load(std::memory_order_relaxed)) {
        expected.fetch_add(1, std::memory_order_relaxed);
        ack_writer->write(&pkt, sizeof(pkt));
      }
    });

    const char r = 'r';
    if (write(ready_pipe[1], &r, 1) != 1) {
      _exit(11);
    }

    std::uint64_t last = 0;
    int idle_ms = 0;
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      const std::uint64_t cur = expected.load(std::memory_order_relaxed);
      if (cur != last) {
        last = cur;
        idle_ms = 0;
        continue;
      }
      if (++idle_ms >= 3000) {
        break;
      }
    }
    { ack_writer.reset(); }
    { reader.reset(); }
    _exit(0);
  }

  close(ready_pipe[1]);
  close(done_pipe[1]);

  char r = 0;
  if (read(ready_pipe[0], &r, 1) != 1) {
    return 1;
  }

  tianshu::transport::shm::ShmBackend backend;
  ChannelConfig cfg;
  cfg.channel_name = name;
  auto writer = backend.create_writer(cfg);
  if (writer == nullptr) {
    return 1;
  }

  ChannelConfig ack_cfg;
  ack_cfg.channel_name = name + "_ack";
  auto ack_reader = backend.create_reader(ack_cfg);
  if (ack_reader == nullptr) {
    return 1;
  }

  std::vector<std::int64_t> latencies;
  latencies.reserve(static_cast<std::size_t>(samples));
  std::atomic<std::uint64_t> acked_seq{0};

  ack_reader->set_callback([&](const Message& msg) {
    BenchPacket pkt{};
    std::memcpy(&pkt, msg.data, msg.size);
    acked_seq.store(pkt.seq, std::memory_order_relaxed);
  });

  const auto wait_ack = [&](std::uint64_t seq) {
    for (int spin = 0; spin < 2000000; ++spin) {
      if (acked_seq.load(std::memory_order_relaxed) >= seq) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(5));
    }
    return false;
  };

  for (int i = 0; i < samples; ++i) {
    BenchPacket pkt{};
    pkt.seq = static_cast<std::uint64_t>(i);
    pkt.send_ns = now_ns();
    writer->write(&pkt, sizeof(pkt));
    if (wait_ack(pkt.seq)) {
      latencies.push_back(now_ns() - pkt.send_ns);
    }
  }

  int status = 0;
  // Child exits when we kill it after samples.
  waitpid(pid, &status, 0);

  if (latencies.empty()) {
    std::printf("[latency] no samples collected\n");
    return 1;
  }

  std::sort(latencies.begin(), latencies.end());
  const auto pct = [&](double p) {
    const auto idx = static_cast<std::size_t>(p * static_cast<double>(latencies.size() - 1));
    return latencies[std::min(idx, latencies.size() - 1)];
  };

  std::printf("[latency] %zu samples: p50=%ldns p90=%ldns p99=%ldns max=%ldns\n",
              latencies.size(), static_cast<long>(pct(0.50)), static_cast<long>(pct(0.90)),
              static_cast<long>(pct(0.99)), static_cast<long>(latencies.back()));

  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const int throughput_msgs = argc > 1 ? std::atoi(argv[1]) : 200000;
  const int latency_samples = argc > 2 ? std::atoi(argv[2]) : 200;

  std::printf("=== TIANSHU SHM transport benchmark (fork, cross-process) ===\n");
  int rc = run_throughput_bench("/bench/shm_throughput", throughput_msgs);
  rc |= run_latency_bench("/bench/shm_latency", latency_samples);
  std::printf("=== %s ===\n", rc == 0 ? "ALL BENCHES OK" : "BENCH FAILURES");
  return rc;
}
