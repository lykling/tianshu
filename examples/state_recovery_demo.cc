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

// State-as-data recovery demo (ADR-0027): a stateful fold publishes
// every state version onto a state channel; after the stream has flown,
// the operator is "crashed" and recovered from an earlier checkpoint —
// the state's lineage names the input it absorbed, so only the suffix
// needs replaying. Recovered final state must equal the uninterrupted
// run exactly.

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

// NOLINTNEXTLINE(misc-use-internal-linkage)  // traits must precede template use
struct EvMsg {
  std::uint64_t t;
  double v;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct AccMsg {
  std::uint64_t n;
  double sum;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct OutMsg {
  std::uint64_t n;
  double val;
};

TIANSHU_TRAITS_POD(EvMsg, "sr.EvMsg");
TIANSHU_TRAITS_POD(AccMsg, "sr.AccMsg");
TIANSHU_TRAITS_POD(OutMsg, "sr.OutMsg");

// NOLINTNEXTLINE(misc-use-internal-linkage)  // wire template needs linkage
class FoldOp {
 public:
  FoldOp() = default;
  FoldOp(std::uint64_t n0, double sum0) : n_(n0), sum_(sum0) {}

  static void on_init(tianshu::dsl::OpPub<OutMsg>& /*out*/, tianshu::dsl::OpPub<AccMsg>& /*st*/) {}

  void handle(const EvMsg& e, tianshu::dsl::OpPub<OutMsg>& out, tianshu::dsl::OpPub<AccMsg>& st) {
    sum_ += e.v;
    ++n_;
    st.publish(AccMsg{.n = n_, .sum = sum_});
    out.publish(OutMsg{.n = n_, .val = sum_});
  }

 private:
  std::uint64_t n_{0};
  double sum_{0.0};
};

namespace {

tianshu::dsl::Flow make_flow(const std::string& name, FoldOp impl,
                             std::vector<std::string>* samples) {
  tianshu::dsl::FlowBuilder builder(name);
  auto ev = builder.source<EvMsg>("ev", std::chrono::milliseconds(5), [](std::uint64_t t) {
    return EvMsg{.t = t, .v = 1.0 + static_cast<double>(t % 7)};
  });
  auto out = builder.stateful<OutMsg, AccMsg>(ev, "out", "acc", impl);
  if (samples != nullptr) {
    out.sink([samples](const OutMsg& o, const tianshu::core::Lineage& lin) {
      if (o.n % 20 == 0 && samples->size() < 16) {
        samples->push_back("n=" + std::to_string(o.n) + " sum=" + std::to_string(o.val) +
                           "  lineage: " + lin.describe());
      }
    });
  }
  return builder.build();
}

}  // namespace

int main() {
  static_cast<void>(std::printf("== state-as-data recovery (ADR-0027) ==\n\n"));

  // Reference: uninterrupted run.
  std::vector<std::string> samples;
  const auto full_flow = make_flow("full", FoldOp{}, &samples);
  tianshu::dsl::FlowRuntime full_rt;
  full_rt.run_for(full_flow, std::chrono::milliseconds(600));
  const auto* full_acc = full_rt.history("full/acc");
  if (full_acc == nullptr || full_acc->entries().empty()) {
    static_cast<void>(std::printf("reference run produced no state\n"));
    return 1;
  }
  AccMsg full_final{};
  std::memcpy(&full_final, full_acc->entries().back().bytes.data(), sizeof(full_final));

  static_cast<void>(std::printf("[reference] absorbed %llu inputs, sum=%.1f\n",
                                static_cast<unsigned long long>(full_final.n), full_final.sum));
  static_cast<void>(std::printf("[versions]\n"));
  for (const auto& s : samples) {
    static_cast<void>(std::printf("  %s\n", s.c_str()));
  }

  // Crashed run + recovery from a mid-stream checkpoint.
  const auto crash_flow = make_flow("cr", FoldOp{}, nullptr);
  tianshu::dsl::FlowRuntime crash_rt;
  crash_rt.run_for(crash_flow, std::chrono::milliseconds(600));
  const auto* acc_hist = crash_rt.history("cr/acc");
  const auto* ev_hist = crash_rt.history("cr/ev");
  if (acc_hist == nullptr || acc_hist->entries().size() < 8 || ev_hist == nullptr) {
    static_cast<void>(std::printf("crashed run history too short\n"));
    return 1;
  }

  // Checkpoint: the oldest state retained in the bounded ring.
  const auto& checkpoint = acc_hist->entries().front();
  AccMsg base{};
  std::memcpy(&base, checkpoint.bytes.data(), sizeof(base));
  const std::uint64_t absorbed = checkpoint.lineage.root().seq;
  static_cast<void>(std::printf(
      "\n[crash] recovering from checkpoint state#%llu (n=%llu) — lineage says inputs "
      "absorbed through ev#%llu\n",
      static_cast<unsigned long long>(checkpoint.seq), static_cast<unsigned long long>(base.n),
      static_cast<unsigned long long>(absorbed)));

  tianshu::dsl::FlowRuntime rec_rt;
  const auto rec_flow = make_flow("rec", FoldOp{base.n, base.sum}, nullptr);
  for (const auto& decl : rec_flow.statefuls()) {
    decl.wire(rec_rt);
  }
  std::uint64_t replayed = 0;
  for (const auto& entry : ev_hist->entries()) {
    if (entry.seq <= absorbed) {
      continue;
    }
    rec_rt.publish_bytes("rec/ev", entry.bytes.data(), entry.bytes.size(),
                         tianshu::core::Lineage::rooted("rec/ev", entry.seq));
    ++replayed;
  }
  const auto* rec_acc = rec_rt.history("rec/acc");
  if (rec_acc == nullptr || rec_acc->entries().empty()) {
    static_cast<void>(std::printf("recovery produced no state\n"));
    return 1;
  }
  AccMsg rec_final{};
  std::memcpy(&rec_final, rec_acc->entries().back().bytes.data(), sizeof(rec_final));

  static_cast<void>(std::printf("[replay] %llu inputs re-published through the recovered op\n",
                                static_cast<unsigned long long>(replayed)));
  static_cast<void>(
      std::printf("[result ] recovered n=%llu sum=%.1f   reference n=%llu sum=%.1f   %s\n",
                  static_cast<unsigned long long>(rec_final.n), rec_final.sum,
                  static_cast<unsigned long long>(full_final.n), full_final.sum,
                  (rec_final.n == full_final.n && rec_final.sum == full_final.sum) ? "EXACT MATCH"
                                                                                   : "MISMATCH"));
  return 0;
}
