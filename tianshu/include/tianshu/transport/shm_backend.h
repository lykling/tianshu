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

// SHM transport backend: cross-process message passing via named segments.
//
// Design (per L4-TRANS-3/4, ADR-0010):
//   - One named segment per channel: [ChannelHeader][slot 0]...[slot N-1]
//   - Each reader owns a slot: [mutex][cond][SPSC ring]
//   - Writer broadcasts to every live slot, signals the slot cond (L4-TRANS-4)
//   - Reader background thread waits on its cond, drains its ring
//   - Slot bitmap in the header handles cross-process reader registration
//   - Linux hosted profiles only (embedded/mcu use INTRA per L4-TRANS-11)

#pragma once

#include <pthread.h>  // NOLINT(misc-include-cleaner)  // glibc: bits/pthreadtypes

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "tianshu/shm/shm_ring.h"
#include "tianshu/shm/shm_segment.h"
#include "tianshu/transport/transport_backend.h"

namespace tianshu::transport::shm {

inline constexpr std::uint32_t kMaxReaders = 8;
inline constexpr std::uint32_t kRingCapacity = 256 * 1024;
inline constexpr std::uint64_t kChannelMagic = 0x4348534d'4348414eULL;

struct ChannelHeader {
  std::atomic<std::uint64_t> magic;
  std::atomic<std::uint32_t> live_slots;
  std::atomic<std::uint64_t> seq;
  std::uint32_t writer_pid;
  std::uint32_t max_readers;
  std::uint32_t ring_capacity;
  std::uint32_t slot_stride;
};

struct SlotHeader {
  pthread_mutex_t mutex;  // NOLINT(misc-include-cleaner)  // glibc: bits/pthreadtypes
  pthread_cond_t cond;    // NOLINT(misc-include-cleaner)  // glibc: bits/pthreadtypes
};

class ShmChannel {
 public:
  static std::unique_ptr<ShmChannel> open(std::string_view channel_name);

  ~ShmChannel();

  ShmChannel(const ShmChannel&) = delete;
  ShmChannel& operator=(const ShmChannel&) = delete;

  [[nodiscard]] std::string_view channel() const { return channel_; }
  [[nodiscard]] ChannelHeader* header() const { return header_; }

  // Slot management (cross-process safe).
  std::int32_t acquire_slot();
  void release_slot(std::int32_t slot);
  void reset_slot(std::int32_t slot);

  SlotHeader* slot_header(std::int32_t slot);
  void* slot_ring_mem(std::int32_t slot);
  ::tianshu::shm::SpscRing slot_ring(std::int32_t slot);
  void signal_slot(std::int32_t slot);

  // Fan-out write to every live slot. Returns count of slots served.
  std::uint32_t broadcast(const void* data, std::size_t size);

 private:
  ShmChannel() = default;
  bool init(std::string_view channel_name);

  std::unique_ptr<::tianshu::shm::ShmSegment> segment_;
  ChannelHeader* header_{nullptr};
  std::string channel_;
};

class ShmWriter : public WriterBase {
 public:
  ShmWriter(std::string channel, std::shared_ptr<ShmChannel> shm_channel)
      : channel_(std::move(channel)), shm_channel_(std::move(shm_channel)) {}

  void write(const void* data, std::size_t size) override;
  std::string_view channel() const override { return channel_; }

 private:
  std::string channel_;
  std::shared_ptr<ShmChannel> shm_channel_;
};

class ShmReader : public ReaderBase {
 public:
  ShmReader(std::string channel, std::shared_ptr<ShmChannel> shm_channel, std::int32_t slot);
  ~ShmReader() override;

  ShmReader(const ShmReader&) = delete;
  ShmReader& operator=(const ShmReader&) = delete;

  void set_callback(MessageCallback cb) override { callback_ = std::move(cb); }
  std::string_view channel() const override { return channel_; }

 private:
  void reader_loop();

  std::string channel_;
  std::shared_ptr<ShmChannel> shm_channel_;
  std::int32_t slot_{-1};
  MessageCallback callback_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

class ShmBackend : public TransportBackend {
 public:
  BackendType type() const override { return BackendType::kShm; }
  bool supports_zero_copy() const override { return false; }
  bool supports_remote() const override { return false; }

  std::unique_ptr<WriterBase> create_writer(const ChannelConfig& cfg) override;
  std::unique_ptr<ReaderBase> create_reader(const ChannelConfig& cfg) override;
};

}  // namespace tianshu::transport::shm
