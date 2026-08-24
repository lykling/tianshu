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

#include "tianshu/transport/shm_backend.h"

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <utility>

namespace tianshu::transport::shm {

namespace {

std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string segment_name_for(std::string_view channel) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (char c : channel) {
    hash ^= static_cast<std::uint8_t>(c);
    hash *= 1099511628211ULL;
  }
  char buf[48];
  std::snprintf(buf, sizeof(buf), "/tianshu_ch_%016llx", static_cast<unsigned long long>(hash));
  return std::string(buf);
}

std::size_t slot_stride_bytes(std::uint32_t ring_capacity) {
  return sizeof(SlotHeader) + ::tianshu::shm::SpscRing::total_size(ring_capacity);
}

std::size_t segment_total_size(std::uint32_t ring_capacity) {
  return sizeof(ChannelHeader) + kMaxReaders * slot_stride_bytes(ring_capacity);
}

bool wait_channel_ready(ChannelHeader* header) {
  for (int i = 0; i < 2000; ++i) {
    if (header->magic.load(std::memory_order_acquire) == kChannelMagic) {
      return true;
    }
    timespec ts{0, 500 * 1000};
    nanosleep(&ts, nullptr);
  }
  return header->magic.load(std::memory_order_acquire) == kChannelMagic;
}

// State machine for header_->magic: 0 = fresh segment, kChannelInitializing
// = a process is setting the channel up, kChannelMagic = ready for use.
constexpr std::uint64_t kChannelInitializing = 0x494e4954'4348534dULL;

void init_slot(void* slot_mem, std::uint32_t ring_capacity) {
  auto* slot = static_cast<SlotHeader*>(slot_mem);

  pthread_mutexattr_t mattr;
  pthread_mutexattr_init(&mattr);
  pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
  pthread_mutex_init(&slot->mutex, &mattr);
  pthread_mutexattr_destroy(&mattr);

  pthread_condattr_t cattr;
  pthread_condattr_init(&cattr);
  pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);
  pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
  pthread_cond_init(&slot->cond, &cattr);
  pthread_condattr_destroy(&cattr);

  ::tianshu::shm::SpscRing::create(static_cast<char*>(slot_mem) + sizeof(SlotHeader),
                                   ring_capacity);
}

}  // namespace

// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)

std::unique_ptr<ShmChannel> ShmChannel::open(std::string_view channel_name) {
  auto channel = std::unique_ptr<ShmChannel>(new ShmChannel());
  if (!channel->init(channel_name)) {
    return nullptr;
  }
  return channel;
}

ShmChannel::~ShmChannel() = default;

bool ShmChannel::init(std::string_view channel_name) {
  const std::string segment_name = segment_name_for(channel_name);
  const std::size_t total = segment_total_size(kRingCapacity);

  segment_ = ::tianshu::shm::ShmSegment::open_or_create(segment_name, total);
  if (segment_ == nullptr) {
    return false;
  }

  auto* raw = static_cast<char*>(segment_->data());
  header_ = reinterpret_cast<ChannelHeader*>(raw);

  std::uint64_t expected = 0;
  if (header_->magic.compare_exchange_strong(expected, kChannelInitializing,
                                             std::memory_order_acq_rel)) {
    header_->live_slots.store(0, std::memory_order_relaxed);
    header_->seq.store(0, std::memory_order_relaxed);
    header_->writer_pid = static_cast<std::uint32_t>(getpid());
    header_->max_readers = kMaxReaders;
    header_->ring_capacity = kRingCapacity;
    for (std::uint32_t i = 0; i < kMaxReaders; ++i) {
      init_slot(raw + sizeof(ChannelHeader) + i * slot_stride_bytes(kRingCapacity), kRingCapacity);
    }
    header_->slot_stride = static_cast<std::uint32_t>(slot_stride_bytes(kRingCapacity));
    header_->magic.store(kChannelMagic, std::memory_order_release);
  } else if (!wait_channel_ready(header_)) {
    return false;
  }

  channel_ = std::string(channel_name);
  return true;
}

std::int32_t ShmChannel::acquire_slot() {
  for (std::uint32_t i = 0; i < header_->max_readers; ++i) {
    const std::uint32_t mask = 1U << i;
    const std::uint32_t old = header_->live_slots.fetch_or(mask, std::memory_order_acq_rel);
    if ((old & mask) == 0) {
      return static_cast<std::int32_t>(i);
    }
  }
  return -1;
}

void ShmChannel::release_slot(std::int32_t slot) {
  if (slot < 0 || slot >= static_cast<std::int32_t>(header_->max_readers)) {
    return;
  }
  header_->live_slots.fetch_and(~(1U << slot), std::memory_order_acq_rel);
}

void ShmChannel::reset_slot(std::int32_t slot) {
  ::tianshu::shm::SpscRing::create(slot_ring_mem(slot), header_->ring_capacity);
}

SlotHeader* ShmChannel::slot_header(std::int32_t slot) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  auto* base = reinterpret_cast<char*>(header_) + sizeof(ChannelHeader);
  return reinterpret_cast<SlotHeader*>(base +
                                       static_cast<std::size_t>(slot) * header_->slot_stride);
}

void* ShmChannel::slot_ring_mem(std::int32_t slot) {
  return static_cast<char*>(static_cast<void*>(slot_header(slot))) + sizeof(SlotHeader);
}

::tianshu::shm::SpscRing ShmChannel::slot_ring(std::int32_t slot) {
  return ::tianshu::shm::SpscRing::attach(slot_ring_mem(slot));
}

void ShmChannel::signal_slot(std::int32_t slot) {
  SlotHeader* slot_hdr = slot_header(slot);
  pthread_mutex_lock(&slot_hdr->mutex);
  pthread_cond_signal(&slot_hdr->cond);
  pthread_mutex_unlock(&slot_hdr->mutex);
}

std::uint32_t ShmChannel::broadcast(const void* data, std::size_t size) {
  const ::tianshu::shm::SpscRing::Metadata meta{
      header_->seq.fetch_add(1, std::memory_order_relaxed), now_ns()};

  const std::uint32_t live = header_->live_slots.load(std::memory_order_acquire);
  std::uint32_t served = 0;
  for (std::uint32_t i = 0; i < header_->max_readers; ++i) {
    if ((live & (1U << i)) == 0) {
      continue;
    }
    if (slot_ring(static_cast<std::int32_t>(i)).push(data, size, meta)) {
      ++served;
    }
    signal_slot(static_cast<std::int32_t>(i));
  }
  return served;
}

// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

void ShmWriter::write(const void* data, std::size_t size) { shm_channel_->broadcast(data, size); }

ShmReader::ShmReader(std::string channel, std::shared_ptr<ShmChannel> shm_channel, int slot)
    : channel_(std::move(channel)),
      shm_channel_(std::move(shm_channel)),
      slot_(slot),
      thread_([this] { reader_loop(); }) {}

ShmReader::~ShmReader() {
  stop_.store(true, std::memory_order_release);
  if (shm_channel_ != nullptr && slot_ >= 0) {
    shm_channel_->signal_slot(slot_);
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  if (shm_channel_ != nullptr && slot_ >= 0) {
    shm_channel_->release_slot(slot_);
  }
}

void ShmReader::reader_loop() {
  SlotHeader* slot_hdr = shm_channel_->slot_header(slot_);
  ::tianshu::shm::SpscRing ring = shm_channel_->slot_ring(slot_);

  std::vector<std::uint8_t> payload;
  ::tianshu::shm::SpscRing::Metadata meta{};

  while (!stop_.load(std::memory_order_acquire)) {
    pthread_mutex_lock(&slot_hdr->mutex);
    if (!stop_.load(std::memory_order_acquire) && ring.empty()) {
      timespec ts{};
      clock_gettime(CLOCK_MONOTONIC, &ts);
      ts.tv_nsec += 100 * 1000 * 1000;
      if (ts.tv_nsec >= 1000 * 1000 * 1000) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000 * 1000 * 1000;
      }
      pthread_cond_timedwait(&slot_hdr->cond, &slot_hdr->mutex, &ts);
    }
    pthread_mutex_unlock(&slot_hdr->mutex);

    while (ring.pop(&payload, &meta)) {
      if (callback_) {
        Message msg;
        msg.data = payload.data();
        msg.size = payload.size();
        msg.seq = meta.seq;
        msg.timestamp_ns = meta.timestamp_ns;
        msg.src_process_id = static_cast<std::uint32_t>(getpid());
        callback_(msg);
      }
    }
  }
}

namespace {

class ShmChannelRegistry {
 public:
  static ShmChannelRegistry& instance() {
    static ShmChannelRegistry registry;
    return registry;
  }

  std::shared_ptr<ShmChannel> get_or_open(std::string_view channel_name) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::string key(channel_name);
    auto it = channels_.find(key);
    if (it != channels_.end()) {
      if (auto existing = it->second.lock()) {
        return existing;
      }
      channels_.erase(it);
    }
    auto channel = ShmChannel::open(channel_name);
    if (channel == nullptr) {
      return nullptr;
    }
    auto shared = std::shared_ptr<ShmChannel>(channel.release());
    channels_.emplace(key, shared);
    return shared;
  }

 private:
  ShmChannelRegistry() = default;
  std::mutex mutex_;
  std::unordered_map<std::string, std::weak_ptr<ShmChannel>> channels_;
};

}  // namespace

std::unique_ptr<WriterBase> ShmBackend::create_writer(const ChannelConfig& cfg) {
  auto channel = ShmChannelRegistry::instance().get_or_open(cfg.channel_name);
  if (channel == nullptr) {
    return nullptr;
  }
  return std::make_unique<ShmWriter>(cfg.channel_name, std::move(channel));
}

std::unique_ptr<ReaderBase> ShmBackend::create_reader(const ChannelConfig& cfg) {
  auto channel = ShmChannelRegistry::instance().get_or_open(cfg.channel_name);
  if (channel == nullptr) {
    return nullptr;
  }
  const int slot = channel->acquire_slot();
  if (slot < 0) {
    return nullptr;
  }
  channel->reset_slot(slot);
  return std::make_unique<ShmReader>(cfg.channel_name, std::move(channel), slot);
}

}  // namespace tianshu::transport::shm
