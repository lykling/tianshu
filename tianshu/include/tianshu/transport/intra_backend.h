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

// INTRA transport backend: zero-copy same-process message passing.
//
// Design (per L4-TRANS-20, ADR-0010):
//   - Writer directly invokes all registered reader callbacks
//   - No serialization, no SHM allocation, no locks on hot path
//   - ~10-100ns per message (function call overhead only)
//   - Auto-selected when reader and writer are in the same process

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "tianshu/transport/transport_backend.h"

namespace tianshu::transport::intra {

class IntraReader;

class IntraWriter : public WriterBase {
 public:
  explicit IntraWriter(std::string channel) : channel_(std::move(channel)) {}

  void write(const void* data, std::size_t size) override {
    Message msg;
    msg.data = data;
    msg.size = size;
    msg.seq = seq_.fetch_add(1, std::memory_order_relaxed);

    const std::scoped_lock lock(callbacks_mutex_);
    for (auto& cb : callbacks_) {
      if (cb) {
        cb(msg);
      }
    }
  }

  std::string_view channel() const override { return channel_; }

  void add_reader_callback(MessageCallback cb) {
    const std::scoped_lock lock(callbacks_mutex_);
    callbacks_.push_back(std::move(cb));
  }

 private:
  std::string channel_;
  std::atomic<uint64_t> seq_{0};
  std::mutex callbacks_mutex_;
  std::vector<MessageCallback> callbacks_;
};

class IntraReader : public ReaderBase {
 public:
  explicit IntraReader(std::string channel, std::shared_ptr<IntraWriter> writer)
      : channel_(std::move(channel)), writer_(std::move(writer)) {
    // Register our callback with the writer.
    writer_->add_reader_callback([this](const Message& msg) {
      if (callback_) {
        callback_(msg);
      }
    });
  }

  void set_callback(MessageCallback cb) override { callback_ = std::move(cb); }
  std::string_view channel() const override { return channel_; }

 private:
  std::string channel_;
  std::shared_ptr<IntraWriter> writer_;
  MessageCallback callback_;
};

// Channel registry: maps channel name to writer (for reader creation).
class IntraChannelRegistry {
 public:
  static IntraChannelRegistry& instance();

  // Returns the channel writer, creating one when absent. Reader attach
  // uses this; the entry alone does NOT count as a real publisher.
  std::shared_ptr<IntraWriter> get_or_create_writer(std::string_view channel);

  // Marks a real publisher for the channel (create_writer path). kAuto
  // readers query has_writer to prefer INTRA over SHM.
  std::shared_ptr<IntraWriter> register_writer(std::string_view channel);

  bool has_writer(std::string_view channel) const;

 private:
  IntraChannelRegistry() = default;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<IntraWriter>> writers_;
  std::unordered_set<std::string> published_;
};

class IntraBackend : public TransportBackend {
 public:
  BackendType type() const override { return BackendType::kIntra; }
  bool supports_zero_copy() const override { return true; }
  bool supports_remote() const override { return false; }

  std::unique_ptr<WriterBase> create_writer(const ChannelConfig& cfg) override {
    auto writer = IntraChannelRegistry::instance().register_writer(cfg.channel_name);
    // Return a new handle that references the shared writer.
    return std::make_unique<IntraWriterRef>(cfg.channel_name, writer);
  }

  std::unique_ptr<ReaderBase> create_reader(const ChannelConfig& cfg) override {
    auto writer = IntraChannelRegistry::instance().get_or_create_writer(cfg.channel_name);
    return std::make_unique<IntraReader>(cfg.channel_name, std::move(writer));
  }

 private:
  // Writer that holds a shared_ptr to the real writer.
  class IntraWriterRef : public WriterBase {
   public:
    IntraWriterRef(std::string channel, std::shared_ptr<IntraWriter> writer)
        : channel_(std::move(channel)), writer_(std::move(writer)) {}

    void write(const void* data, std::size_t size) override { writer_->write(data, size); }
    std::string_view channel() const override { return channel_; }

   private:
    std::string channel_;
    std::shared_ptr<IntraWriter> writer_;
  };
};

}  // namespace tianshu::transport::intra
