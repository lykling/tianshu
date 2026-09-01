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

// Record/replay demo (ADR-0026 Phase C): run a flow live, persist its
// channel histories to a record file, then replay the file through a
// FRESH runtime — the same graph, the same slice queries, but the
// substrate is a file. Replay reproduces the live outputs exactly
// (offline = online with one API).

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "tianshu/core/lineage.h"
#include "tianshu/core/message_traits.h"
#include "tianshu/dsl/dsl_runtime.h"
#include "tianshu/dsl/flow.h"
#include "tianshu/dsl/record.h"

// NOLINTNEXTLINE(misc-use-internal-linkage)  // traits must precede template use
struct TickMsg {
  std::uint64_t tick;
  double v;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct SqMsg {
  std::uint64_t tick;
  double sq;
};

TIANSHU_TRAITS_POD(TickMsg, "rp.TickMsg");
TIANSHU_TRAITS_POD(SqMsg, "rp.SqMsg");

namespace {

tianshu::dsl::Flow build_flow(const std::string& name, std::vector<SqMsg>* sink_out,
                              std::vector<std::string>* sink_lineage) {
  tianshu::dsl::FlowBuilder builder(name);
  auto src = builder.source<TickMsg>("tick", std::chrono::milliseconds(5), [](std::uint64_t t) {
    return TickMsg{.tick = t, .v = 0.5 * static_cast<double>(t)};
  });
  auto sq =
      src.map<SqMsg>([](const TickMsg& in) { return SqMsg{.tick = in.tick, .sq = in.v * in.v}; });
  sq.sink([sink_out, sink_lineage](const SqMsg& msg, const tianshu::core::Lineage& lin) {
    if (sink_out != nullptr) {
      sink_out->push_back(msg);
    }
    if (sink_lineage != nullptr) {
      sink_lineage->push_back(lin.describe());
    }
  });
  return builder.build();
}

}  // namespace

int main() {
  static_cast<void>(std::printf("== record/replay: offline == online (ADR-0026-C) ==\n\n"));

  // Live run (scoped: the runtime must destruct before replay wires —
  // DataDispatcher is a process-wide singleton and dead visitors would
  // double-fire the replay cascade).
  std::vector<SqMsg> live_out;
  bool saved = false;
  {
    const auto live_flow = build_flow("live", &live_out, nullptr);
    tianshu::dsl::FlowRuntime live_rt;
    live_rt.run_for(live_flow, std::chrono::milliseconds(200));
    saved = live_rt.record_to("/tmp/tianshu_record_demo.bin");
  }
  static_cast<void>(
      std::printf("[live] %zu outputs, record saved=%s\n", live_out.size(), saved ? "yes" : "no"));

  // Replay through a FRESH runtime with the same graph.
  const auto records = tianshu::dsl::RecordFile::load("/tmp/tianshu_record_demo.bin");
  static_cast<void>(std::printf("[file] %zu messages loaded\n", records.size()));

  std::vector<SqMsg> replay_out;
  std::vector<std::string> replay_lineage;
  const auto replay_flow = build_flow("live", &replay_out, &replay_lineage);
  tianshu::dsl::FlowRuntime replay_rt;
  for (const auto& decl : replay_flow.maps()) {
    decl.wire(replay_rt);
  }
  for (const auto& decl : replay_flow.sinks()) {
    decl.wire(replay_rt);
  }
  // Replay SOURCE-channel messages only: intermediate channels recompute
  // through the live cascade (the whole point — offline == online).
  std::vector<tianshu::dsl::RecordedMessage> source_records;
  for (const auto& rec : records) {
    if (rec.channel == "live/tick") {
      source_records.push_back(rec);
    }
  }
  replay_rt.replay_from(source_records);

  // Compare.
  static_cast<void>(std::printf("[replay] %zu outputs\n", replay_out.size()));
  std::size_t matches = 0;
  for (std::size_t i = 0; i < live_out.size() && i < replay_out.size(); ++i) {
    if (live_out[i].tick == replay_out[i].tick && live_out[i].sq == replay_out[i].sq) {
      ++matches;
    }
  }
  static_cast<void>(
      std::printf("[result] %zu/%zu outputs bit-identical\n", matches,
                  live_out.size() < replay_out.size() ? replay_out.size() : live_out.size()));
  if (!replay_lineage.empty()) {
    static_cast<void>(std::printf("[lineage] %s\n", replay_lineage.back().c_str()));
  }
  return 0;
}
