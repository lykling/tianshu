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

// Runtime coverage tests: the live-recording publish path, the
// recording lifecycle API, and wire() over stateful/span stages —
// branches the chain-shaped tests never enter.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "tianshu/core/lineage.h"
#include "tianshu/core/message_traits.h"
#include "tianshu/dsl/dsl_runtime.h"
#include "tianshu/dsl/flow.h"
#include "tianshu/dsl/record_v2.h"

// NOLINTNEXTLINE(misc-use-internal-linkage)  // traits must precede template use
struct RtTick {
  std::uint64_t tick;
};
// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct RtOut {
  std::uint64_t tick;
};
// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct RtState {
  std::uint64_t n{0};
};

TIANSHU_TRAITS_POD(RtTick, "rt.TickMsg");
TIANSHU_TRAITS_POD(RtOut, "rt.OutMsg");
TIANSHU_TRAITS_POD(RtState, "rt.StateMsg");

namespace {

namespace dsl = tianshu::dsl;
namespace record = tianshu::dsl::record;

std::string tmpdir() {
  const char* tmp = std::getenv("TEST_TMPDIR");
  return tmp != nullptr ? std::string(tmp) : std::string("/tmp");
}

TEST(RuntimeCoverageTest, LiveRecordingCapturesCascade) {
  dsl::FlowBuilder b("rt_rec");
  b.source<RtTick>("t", std::chrono::milliseconds(2),
                   [](std::uint64_t t) { return RtTick{.tick = t}; })
      .map<RtOut>([](const RtTick& in) { return RtOut{.tick = in.tick}; })
      .sink([](const RtOut&, const tianshu::core::Lineage&) {});

  const auto flow = b.build();
  dsl::FlowRuntime rt;
  EXPECT_FALSE(rt.is_recording());

  const std::string path = tmpdir() + "/rt_live.trec";
  rt.start_recording(path, record::Compression::kLz4);
  EXPECT_TRUE(rt.is_recording());
  rt.run_for(flow, std::chrono::milliseconds(40));
  EXPECT_TRUE(rt.stop_recording());
  EXPECT_FALSE(rt.is_recording());
  EXPECT_FALSE(rt.stop_recording());  // double-stop: clean no-op

  auto reader = record::RecordReader::open(path);
  ASSERT_TRUE(reader.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)  // guarded by the assert
}

TEST(RuntimeCoverageTest, StatefulAndSpanStagesWireThroughRunFor) {
  dsl::FlowBuilder b("rt_ss");
  const auto data = b.tap<RtTick>("d");
  const auto trig = b.tap<RtTick>("g");

  std::uint64_t folded = 0;
  struct Fold2 {
    static void on_init(dsl::OpPub<RtOut>& /*out*/, dsl::OpPub<RtState>& /*state*/) {}
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    void handle(const RtTick& in, dsl::OpPub<RtOut>& out, dsl::OpPub<RtState>& /*state*/) {
      out.publish(RtOut{.tick = in.tick});
    }
  };
  auto fold_out = b.stateful<RtOut, RtState>(data, "fold_out", "fold_acc", Fold2{});
  fold_out.sink([&folded](const RtOut& msg, const tianshu::core::Lineage&) { folded += msg.tick; });

  auto comp = b.span_join<RtOut>(
      trig, data,
      [](const RtTick& trig_msg) {
        return std::pair<std::uint64_t, std::uint64_t>(trig_msg.tick, trig_msg.tick + 10);
      },
      [](const RtTick& msg) { return msg.tick; },
      [](const RtTick& trig_msg, const dsl::Slice<RtTick>& slice) {
        static_cast<void>(trig_msg);
        std::uint64_t sum = 0;
        for (const auto& item : slice.items) {
          sum += item.tick;
        }
        return RtOut{.tick = sum};
      });
  comp.sink([&folded](const RtOut& msg, const tianshu::core::Lineage&) { folded += msg.tick; });

  // Drive both taps from a source so the cascade actually fires.
  b.source<RtTick>("d", std::chrono::milliseconds(2),
                   [](std::uint64_t t) { return RtTick{.tick = t}; });
  b.source<RtTick>("g", std::chrono::milliseconds(4),
                   [](std::uint64_t t) { return RtTick{.tick = t}; });

  const auto flow = b.build();
  dsl::FlowRuntime rt;
  rt.run_for(flow, std::chrono::milliseconds(60));
  EXPECT_GT(folded, 0U);
}

}  // namespace

// from() with an unknown registration: the chain is invalid, and wiring
// a flow containing it walks the attach failure branches (nullptr
// component -> early return) without crashing.
TEST(RuntimeCoverageTest, UnknownFromReferenceWiresToNoop) {
  dsl::FlowBuilder b("rt_badfrom");
  auto chain = b.from<RtTick>("no_such_component", "ghost/out", std::chrono::milliseconds(5));
  ASSERT_FALSE(chain.valid());
  chain.sink([](const RtTick&, const tianshu::core::Lineage&) {});
  const auto flow = b.build();
  dsl::FlowRuntime rt;
  rt.run_for(flow, std::chrono::milliseconds(30));
  SUCCEED();  // no crash, no hang
}
