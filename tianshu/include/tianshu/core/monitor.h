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

// Monitor core: channel observation with pause-and-browse semantics.
//
// Design (ti-monitor, cyber_monitor-equivalent):
//   - MonitorBuffer caches the last N frames per channel
//   - LIVE: buffer rolls (ring overwrite), view = latest frame
//   - PAUSE: buffer locks (new frames are dropped), cursor supports
//     single-step and jump browsing over the cached frames
//   - RESUME: buffer rolls again, view snaps back to the latest frame
//   - TUI-free core so the semantics are unit-testable; the terminal
//     layer (ti-monitor) only renders snapshots

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "tianshu/transport/transport_backend.h"

namespace tianshu::core {

struct MonitorFrame {
  std::vector<std::uint8_t> payload;
  std::uint64_t seq{0};
  std::int64_t timestamp_ns{0};
};

class MonitorBuffer {
 public:
  explicit MonitorBuffer(std::size_t capacity);

  // LIVE: append (overwriting the oldest when full). PAUSE: drop.
  void push(const MonitorFrame& frame);

  void pause();
  void resume();
  [[nodiscard]] bool paused() const;

  // Frame navigation; no-ops in LIVE mode. Returns false when paused-off
  // or the buffer is empty. `delta` may be negative.
  bool step(std::int64_t delta);
  bool jump_to_first();
  bool jump_to_last();
  bool jump_by(std::int64_t delta);

  // Absolute cursor (0-based over valid frames). Always the latest in LIVE.
  [[nodiscard]] std::size_t cursor() const;
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::size_t total_pushed() const;

  // View the current frame (nullptr when empty). Copy semantics for
  // cross-thread rendering; payloads are small.
  [[nodiscard]] MonitorFrame view() const;

 private:
  std::size_t slot_of(std::size_t absolute_index) const;

  std::vector<MonitorFrame> ring_;
  std::size_t capacity_;
  std::size_t pushed_{0};
  std::size_t cursor_abs_{0};
  bool paused_{false};
  mutable std::mutex mutex_;
};

struct MonitorChannelStats {
  double hz{0};
  std::uint64_t last_seq{0};
  std::size_t last_size{0};
  std::uint64_t msg_count{0};
};

class MonitorChannel {
 public:
  MonitorChannel(std::string name, std::size_t buffer_depth,
                 std::unique_ptr<transport::ReaderBase> reader);

  // Feeds the buffer + stats from the transport callback thread.
  void on_message(const transport::Message& msg);

  MonitorBuffer& buffer();
  [[nodiscard]] const std::string& name() const;
  [[nodiscard]] MonitorChannelStats stats() const;

 private:
  void recompute_hz_locked(std::chrono::steady_clock::time_point now);

  std::string name_;
  std::unique_ptr<transport::ReaderBase> reader_;
  MonitorBuffer buffer_;
  mutable std::mutex stats_mutex_;
  MonitorChannelStats stats_;
  std::vector<std::chrono::steady_clock::time_point> arrivals_;
  std::size_t arrival_cursor_{0};
};

// One snapshot for the whole UI (channels + selected frame) taken under
// one lock acquisition per channel; keeps drawing lock-free.
struct MonitorUiSnapshot {
  struct ChannelView {
    std::string name;
    MonitorChannelStats stats;
    bool paused{false};
    std::size_t cursor{0};
    std::size_t buffered{0};
    std::uint64_t total_pushed{0};
  };
  std::vector<ChannelView> channels;
  std::size_t selected{0};
  bool app_paused{false};
  MonitorFrame frame;  // selected channel's current view (may be empty)
};

class MonitorApp {
 public:
  explicit MonitorApp(std::size_t buffer_depth = 512);

  // Attaches an SHM reader to the channel; returns false on transport
  // failure.
  bool add_channel(std::string_view channel);

  // Global pause (locks every channel's buffer); browsing acts on the
  // selected channel.
  void pause();
  void resume();
  [[nodiscard]] bool paused() const;

  void select(std::size_t index);
  void select_delta(std::int64_t delta);
  // Navigation on the selected channel's buffer.
  bool step_frame(std::int64_t delta);
  bool jump_frame_by(std::int64_t delta);
  bool jump_frame_first();
  bool jump_frame_last();

  [[nodiscard]] MonitorUiSnapshot snapshot();
  [[nodiscard]] std::size_t channel_count() const;

  // Waits until every registered channel has at least one frame or the
  // timeout expires. Returns the number of channels still empty.
  std::size_t wait_first_frames(std::chrono::milliseconds timeout);

 private:
  std::size_t buffer_depth_;
  std::vector<std::unique_ptr<MonitorChannel>> channels_;
  std::size_t selected_{0};
  std::atomic<bool> paused_{false};
};

}  // namespace tianshu::core
