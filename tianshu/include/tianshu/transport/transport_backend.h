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

// Transport backend abstraction layer.
//
// Design (per L4-TRANS-18, ADR-0010):
//   - All transport implementations (INTRA / SHM / Zenoh) implement this interface
//   - HybridTransport selects the best backend per channel
//   - Users/compilers don't need to know which backend is active

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tianshu::transport {

enum class BackendType : uint8_t {
  kIntra,
  kShm,
  kZenoh,
  kCustom,
};

enum class MessageFormat : uint8_t {
  kPod,
  kFlatBuffers,
  kProtobuf,
};

enum class TransportMode : uint8_t {
  kIntra,
  kShm,
  kAuto,
};

struct ChannelConfig {
  std::string channel_name;
  std::string msg_type_name;
  std::size_t queue_size{16};
  MessageFormat format{MessageFormat::kPod};
  // Optional schema blob (ADR-0020): published beside the channel by
  // backends that support it (SHM sidecar); empty = no schema.
  std::vector<std::uint8_t> schema_blob;
};

// Opaque message payload (raw bytes or struct pointer, backend-dependent).
struct Message {
  const void* data{nullptr};
  std::size_t size{0};
  uint64_t seq{0};
  int64_t timestamp_ns{0};
  uint32_t src_process_id{0};
  const void* lineage_ptr{nullptr};
};

using MessageCallback = std::function<void(const Message&)>;

// Writer endpoint: sends messages to a channel.
class WriterBase {
 public:
  virtual ~WriterBase() = default;
  virtual void write(const void* data, std::size_t size) = 0;

  // Lineage-carrying write (ADR-0025 correction): the pointer must
  // point to a core::Lineage that outlives the synchronous write; the
  // default drops it (backends without lineage support).
  virtual void write(const void* data, std::size_t size, const void* /*lineage_ptr*/) {
    write(data, size);
  }
  virtual std::string_view channel() const = 0;
};

// Reader endpoint: receives messages from a channel.
class ReaderBase {
 public:
  virtual ~ReaderBase() = default;
  virtual void set_callback(MessageCallback cb) = 0;
  virtual std::string_view channel() const = 0;
};

// Backend interface: creates writers and readers for channels.
class TransportBackend {
 public:
  virtual ~TransportBackend() = default;

  virtual BackendType type() const = 0;
  virtual bool supports_zero_copy() const = 0;
  virtual bool supports_remote() const = 0;

  virtual std::unique_ptr<WriterBase> create_writer(const ChannelConfig& cfg) = 0;
  virtual std::unique_ptr<ReaderBase> create_reader(const ChannelConfig& cfg) = 0;
};

// Registry for backend lookup (per L4-TRANS-18).
class TransportRegistry {
 public:
  static TransportRegistry& instance();

  void register_backend(BackendType type,
                        const std::function<std::unique_ptr<TransportBackend>()>& factory);

  TransportBackend* get(BackendType type);

 private:
  TransportRegistry() = default;
  std::unordered_map<BackendType, std::unique_ptr<TransportBackend>> backends_;
};

}  // namespace tianshu::transport
