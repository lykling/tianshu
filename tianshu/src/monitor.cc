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

#include "tianshu/core/monitor.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "tianshu/core/field_table.h"
#include "tianshu/core/node.h"
#include "tianshu/transport/schema_sidecar.h"

namespace tianshu::core {

// ---------------------------------------------------------------------------
// MonitorBuffer
// ---------------------------------------------------------------------------

MonitorBuffer::MonitorBuffer(std::size_t capacity) : ring_(capacity), capacity_(capacity) {}

void MonitorBuffer::push(const MonitorFrame& frame) {
  const std::scoped_lock lock(mutex_);
  if (paused_) {
    return;
  }
  ring_[pushed_ % capacity_] = frame;
  ++pushed_;
  cursor_abs_ = pushed_ - 1;
}

void MonitorBuffer::pause() {
  const std::scoped_lock lock(mutex_);
  paused_ = true;
  cursor_abs_ = pushed_ == 0 ? 0 : pushed_ - 1;
}

void MonitorBuffer::resume() {
  const std::scoped_lock lock(mutex_);
  paused_ = false;
  cursor_abs_ = pushed_ == 0 ? 0 : pushed_ - 1;
}

bool MonitorBuffer::paused() const {
  const std::scoped_lock lock(mutex_);
  return paused_;
}

std::size_t MonitorBuffer::slot_of(std::size_t absolute_index) const {
  return absolute_index % capacity_;
}

bool MonitorBuffer::step(std::int64_t delta) { return jump_by(delta); }

bool MonitorBuffer::jump_by(std::int64_t delta) {
  const std::scoped_lock lock(mutex_);
  if (!paused_ || pushed_ == 0) {
    return false;
  }
  const std::size_t valid = std::min(pushed_, capacity_);
  const std::size_t oldest = pushed_ - valid;
  const std::int64_t target = std::clamp<std::int64_t>(
      static_cast<std::int64_t>(cursor_abs_) + delta, static_cast<std::int64_t>(oldest),
      static_cast<std::int64_t>(pushed_) - 1);
  cursor_abs_ = static_cast<std::size_t>(target);
  return true;
}

bool MonitorBuffer::jump_to_first() {
  const std::scoped_lock lock(mutex_);
  if (!paused_ || pushed_ == 0) {
    return false;
  }
  cursor_abs_ = pushed_ - std::min(pushed_, capacity_);
  return true;
}

bool MonitorBuffer::jump_to_last() {
  const std::scoped_lock lock(mutex_);
  if (!paused_ || pushed_ == 0) {
    return false;
  }
  cursor_abs_ = pushed_ - 1;
  return true;
}

std::size_t MonitorBuffer::cursor() const {
  const std::scoped_lock lock(mutex_);
  if (pushed_ == 0) {
    return 0;
  }
  const std::size_t valid = std::min(pushed_, capacity_);
  return cursor_abs_ - (pushed_ - valid);
}

std::size_t MonitorBuffer::size() const {
  const std::scoped_lock lock(mutex_);
  return std::min(pushed_, capacity_);
}

std::size_t MonitorBuffer::total_pushed() const {
  const std::scoped_lock lock(mutex_);
  return pushed_;
}

MonitorFrame MonitorBuffer::view() const {
  const std::scoped_lock lock(mutex_);
  if (pushed_ == 0) {
    return {};
  }
  return ring_[slot_of(cursor_abs_)];
}

// ---------------------------------------------------------------------------
// MonitorChannel
// ---------------------------------------------------------------------------

MonitorChannel::MonitorChannel(
    std::string name, std::size_t buffer_depth,
    // NOLINTNEXTLINE(misc-include-cleaner)  // transport_backend.h transitively
    std::unique_ptr<transport::ReaderBase> reader)
    : name_(std::move(name)), reader_(std::move(reader)), buffer_(buffer_depth) {
  arrivals_.assign(128, std::chrono::steady_clock::time_point{});
  reader_->set_callback(
      [this](const transport::Message& msg) { on_message(msg); });  // NOLINT(misc-include-cleaner)
}

void MonitorChannel::on_message(const transport::Message& msg) {
  MonitorFrame frame;
  const auto* bytes = static_cast<const std::uint8_t*>(msg.data);
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  frame.payload.assign(bytes, bytes + msg.size);
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  frame.seq = msg.seq;
  frame.timestamp_ns = msg.timestamp_ns;
  buffer_.push(frame);

  const std::scoped_lock lock(stats_mutex_);
  stats_.last_seq = msg.seq;
  stats_.last_size = msg.size;
  ++stats_.msg_count;
  arrivals_[arrival_cursor_ % arrivals_.size()] = std::chrono::steady_clock::now();
  ++arrival_cursor_;
  recompute_hz_locked(std::chrono::steady_clock::now());
}

void MonitorChannel::recompute_hz_locked(std::chrono::steady_clock::time_point now) {
  const auto window = std::chrono::milliseconds(1000);
  std::size_t recent = 0;
  for (const auto& t : arrivals_) {
    if (t.time_since_epoch().count() != 0 && now - t <= window) {
      ++recent;
    }
  }
  stats_.hz = static_cast<double>(recent);
}

MonitorBuffer& MonitorChannel::buffer() { return buffer_; }

const std::string& MonitorChannel::name() const { return name_; }

MonitorChannelStats MonitorChannel::stats() const {
  const std::scoped_lock lock(stats_mutex_);
  return stats_;
}

// ---------------------------------------------------------------------------
// MonitorApp
// ---------------------------------------------------------------------------

MonitorApp::MonitorApp(std::size_t buffer_depth) : buffer_depth_(buffer_depth) {}

bool MonitorApp::add_channel(std::string_view channel) {
  Node node(transport::TransportMode::kShm);  // NOLINT(misc-include-cleaner)
  auto reader = node.create_reader(channel);
  if (reader == nullptr) {
    return false;
  }
  auto mon =
      std::make_unique<MonitorChannel>(std::string(channel), buffer_depth_, std::move(reader));
  std::vector<std::uint8_t> blob;
  if (transport::shm::read_channel_schema(channel, &blob)) {
    OwnedSchemaTable table;
    if (decode_pod_schema(blob.data(), blob.size(), &table)) {
      mon->set_schema_type_name(table.type_name);
      DecoderRegistry::instance().register_schema(std::move(table));
    }
  }
  channels_.push_back(std::move(mon));
  return true;
}

void MonitorApp::pause() {
  paused_.store(true, std::memory_order_release);
  for (auto& ch : channels_) {
    ch->buffer().pause();
  }
}

void MonitorApp::resume() {
  paused_.store(false, std::memory_order_release);
  for (auto& ch : channels_) {
    ch->buffer().resume();
  }
}

bool MonitorApp::paused() const { return paused_.load(std::memory_order_acquire); }

void MonitorApp::select(std::size_t index) {
  if (!channels_.empty()) {
    selected_ = std::min(index, channels_.size() - 1);
  }
}

void MonitorApp::select_delta(std::int64_t delta) {
  if (channels_.empty()) {
    return;
  }
  const std::int64_t target =
      std::clamp<std::int64_t>(static_cast<std::int64_t>(selected_) + delta, 0,
                               static_cast<std::int64_t>(channels_.size()) - 1);
  selected_ = static_cast<std::size_t>(target);
}

bool MonitorApp::step_frame(std::int64_t delta) {
  if (selected_ >= channels_.size()) {
    return false;
  }
  return channels_[selected_]->buffer().step(delta);
}

bool MonitorApp::jump_frame_by(std::int64_t delta) {
  if (selected_ >= channels_.size()) {
    return false;
  }
  return channels_[selected_]->buffer().jump_by(delta);
}

bool MonitorApp::jump_frame_first() {
  if (selected_ >= channels_.size()) {
    return false;
  }
  return channels_[selected_]->buffer().jump_to_first();
}

bool MonitorApp::jump_frame_last() {
  if (selected_ >= channels_.size()) {
    return false;
  }
  return channels_[selected_]->buffer().jump_to_last();
}

MonitorUiSnapshot MonitorApp::snapshot() {
  MonitorUiSnapshot snap;
  snap.selected = selected_;
  snap.app_paused = paused_.load(std::memory_order_acquire);
  snap.channels.reserve(channels_.size());
  for (const auto& ch : channels_) {
    MonitorUiSnapshot::ChannelView view;
    view.name = ch->name();
    view.stats = ch->stats();
    view.paused = ch->buffer().paused();
    view.cursor = ch->buffer().cursor();
    view.buffered = ch->buffer().size();
    view.total_pushed = ch->buffer().total_pushed();
    view.schema_type_name = ch->schema_type_name();
    snap.channels.push_back(std::move(view));
  }
  if (selected_ < channels_.size()) {
    snap.frame = channels_[selected_]->buffer().view();
  }
  return snap;
}

std::size_t MonitorApp::channel_count() const { return channels_.size(); }

std::size_t MonitorApp::wait_first_frames(std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    std::size_t empty = 0;
    for (const auto& ch : channels_) {
      if (ch->buffer().total_pushed() == 0) {
        ++empty;
      }
    }
    if (empty == 0) {
      return 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::size_t empty = 0;
  for (const auto& ch : channels_) {
    if (ch->buffer().total_pushed() == 0) {
      ++empty;
    }
  }
  return empty;
}

}  // namespace tianshu::core
