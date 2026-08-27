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

// Unit tests for the ti-monitor core (buffer semantics + app wiring).

#include "tianshu/core/monitor.h"

#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <sys/wait.h>

#include "tianshu/core/node.h"
#include "tianshu/transport/transport_backend.h"

namespace {

using tianshu::core::MonitorBuffer;
using tianshu::core::MonitorFrame;

MonitorFrame frame_of(std::uint64_t seq) {
  MonitorFrame f;
  f.seq = seq;
  f.timestamp_ns = static_cast<std::int64_t>(seq);
  f.payload = {static_cast<std::uint8_t>(seq), static_cast<std::uint8_t>(seq >> 8)};
  return f;
}

TEST(MonitorBufferTest, LiveViewIsLatestAndRingOverwrites) {
  MonitorBuffer buf(4);
  for (std::uint64_t s = 0; s < 6; ++s) {
    buf.push(frame_of(s));
  }
  EXPECT_EQ(buf.size(), 4U);
  EXPECT_EQ(buf.total_pushed(), 6U);
  EXPECT_EQ(buf.view().seq, 5U);
  EXPECT_EQ(buf.cursor(), 3U);  // latest of the 4 valid frames
}

TEST(MonitorBufferTest, PauseLocksBufferAndDropsNewFrames) {
  MonitorBuffer buf(8);
  for (std::uint64_t s = 0; s < 3; ++s) {
    buf.push(frame_of(s));
  }
  buf.pause();
  buf.push(frame_of(99));
  EXPECT_EQ(buf.size(), 3U);
  EXPECT_EQ(buf.view().seq, 2U);
}

TEST(MonitorBufferTest, StepBackAndForwardWhilePaused) {
  MonitorBuffer buf(8);
  for (std::uint64_t s = 0; s < 5; ++s) {
    buf.push(frame_of(s));
  }
  buf.pause();

  ASSERT_TRUE(buf.step(-2));
  EXPECT_EQ(buf.view().seq, 2U);
  ASSERT_TRUE(buf.step(-10));  // clamps at the oldest cached frame
  EXPECT_EQ(buf.view().seq, 0U);
  EXPECT_EQ(buf.cursor(), 0U);
  ASSERT_TRUE(buf.step(3));
  EXPECT_EQ(buf.view().seq, 3U);
  ASSERT_TRUE(buf.step(99));  // clamps at the latest
  EXPECT_EQ(buf.view().seq, 4U);
}

TEST(MonitorBufferTest, JumpFirstLastAndBy) {
  MonitorBuffer buf(4);
  for (std::uint64_t s = 0; s < 10; ++s) {
    buf.push(frame_of(s));
  }
  buf.pause();  // valid frames are seq 6..9

  ASSERT_TRUE(buf.jump_to_first());
  EXPECT_EQ(buf.view().seq, 6U);
  ASSERT_TRUE(buf.jump_by(-5));  // clamps to seq 6
  EXPECT_EQ(buf.view().seq, 6U);
  ASSERT_TRUE(buf.jump_to_last());
  EXPECT_EQ(buf.view().seq, 9U);
}

TEST(MonitorBufferTest, NavigationIsNoopInLiveMode) {
  MonitorBuffer buf(4);
  for (std::uint64_t s = 0; s < 3; ++s) {
    buf.push(frame_of(s));
  }
  EXPECT_FALSE(buf.step(-1));
  EXPECT_FALSE(buf.jump_to_first());
  EXPECT_FALSE(buf.jump_to_last());
  EXPECT_EQ(buf.view().seq, 2U);  // still live
}

TEST(MonitorBufferTest, ResumeSnapsToLatestAndRollsAgain) {
  MonitorBuffer buf(4);
  for (std::uint64_t s = 0; s < 3; ++s) {
    buf.push(frame_of(s));
  }
  buf.pause();
  ASSERT_TRUE(buf.step(-2));
  EXPECT_EQ(buf.view().seq, 0U);

  buf.resume();
  EXPECT_EQ(buf.view().seq, 2U);  // snapped back to latest
  buf.push(frame_of(3));
  EXPECT_EQ(buf.view().seq, 3U);  // buffer rolls again
}

TEST(MonitorBufferTest, EmptyBufferBehavior) {
  MonitorBuffer buf(4);
  EXPECT_EQ(buf.view().seq, 0U);
  EXPECT_EQ(buf.view().payload.size(), 0U);
  buf.pause();
  EXPECT_FALSE(buf.step(1));
  buf.resume();
  EXPECT_FALSE(buf.paused());
}

TEST(MonitorBufferTest, PauseOnEmptyThenFillAfterResume) {
  MonitorBuffer buf(4);
  buf.pause();
  buf.push(frame_of(7));  // dropped while locked
  EXPECT_EQ(buf.size(), 0U);
  buf.resume();
  buf.push(frame_of(8));
  EXPECT_EQ(buf.size(), 1U);
  EXPECT_EQ(buf.view().seq, 8U);
}

struct Imu {
  double ax;
  double az;
};

// End to end: forked SHM publisher -> MonitorApp with an SHM reader.
TEST(MonitorAppTest, AttachesToShmChannelAndSeesFrames) {
  const char* channel = "/monitor/e2e";
  const pid_t pid = fork();  // NOLINT(misc-include-cleaner)  // glibc: sys/types + unistd
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    tianshu::core::Node node(tianshu::transport::TransportMode::kShm);
    auto writer = node.create_writer(channel);
    if (writer == nullptr) {
      _exit(10);
    }
    for (int i = 0; i < 200; ++i) {
      const Imu imu{.ax = i * 0.5, .az = 9.81};
      writer->write(&imu, sizeof(imu));
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    { writer.reset(); }
    _exit(0);
  }

  tianshu::core::MonitorApp app(64);
  ASSERT_TRUE(app.add_channel(channel));
  // Give the publisher a head start so the channel exists.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(app.wait_first_frames(std::chrono::milliseconds(5000)), 0U);

  auto snap = app.snapshot();
  ASSERT_EQ(snap.channels.size(), 1U);
  EXPECT_GT(snap.channels[0].stats.msg_count, 0U);
  EXPECT_EQ(snap.channels[0].stats.last_size, sizeof(Imu));
  EXPECT_GT(snap.channels[0].stats.hz, 0.0);
  EXPECT_EQ(snap.frame.payload.size(), sizeof(Imu));

  app.pause();
  const std::uint64_t locked_seq = snap.frame.seq;
  auto paused_snap = app.snapshot();
  EXPECT_TRUE(paused_snap.app_paused);
  EXPECT_TRUE(app.step_frame(-1));
  auto stepped = app.snapshot();
  EXPECT_LT(stepped.frame.seq, locked_seq);
  app.resume();
  EXPECT_FALSE(app.snapshot().app_paused);

  int status = 0;
  kill(pid, SIGTERM);  // NOLINT(misc-include-cleaner)  // glibc: signal.h
  waitpid(pid, &status, 0);
}

}  // namespace
