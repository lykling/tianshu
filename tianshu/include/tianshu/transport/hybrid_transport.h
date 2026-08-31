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

// Hybrid transport: selects the backend per channel.
//
// Design (per L4-TRANS-19, ADR-0010; kAuto semantics per ADR-0023):
//   - kIntra: same process (zero-copy, default)
//   - kShm: same machine, cross process
//   - kAuto: writers dual-publish (INTRA fan-out + SHM broadcast, which is
//     near-free with zero SHM readers); readers pick INTRA when this
//     process has a real writer on the channel, else SHM. Correct for any
//     reader/writer creation order; no discovery service needed (v0).

#pragma once

#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>

#include "tianshu/transport/intra_backend.h"
#include "tianshu/transport/shm_backend.h"
#include "tianshu/transport/transport_backend.h"

namespace tianshu::transport {

class HybridTransport : public TransportBackend {
 public:
  explicit HybridTransport(TransportMode mode = TransportMode::kIntra)
      : mode_(mode),
        intra_(std::make_unique<intra::IntraBackend>()),
        shm_(std::make_unique<shm::ShmBackend>()) {}

  BackendType type() const override {
    return mode_ == TransportMode::kShm ? BackendType::kShm : BackendType::kIntra;
  }
  bool supports_zero_copy() const override { return mode_ != TransportMode::kShm; }
  bool supports_remote() const override { return false; }

  std::unique_ptr<WriterBase> create_writer(const ChannelConfig& cfg) override;
  std::unique_ptr<ReaderBase> create_reader(const ChannelConfig& cfg) override;

 private:
  TransportBackend* active_backend(const ChannelConfig& cfg);

  TransportMode mode_;
  std::unique_ptr<TransportBackend> intra_;
  std::unique_ptr<TransportBackend> shm_;
};

// kAuto writer: dual-publishes on both backends so that intra readers
// (same process) and SHM readers (cross process, any creation order)
// are both served without a discovery service.
class AutoWriter : public WriterBase {
 public:
  AutoWriter(std::unique_ptr<WriterBase> intra, std::unique_ptr<WriterBase> shm)
      : intra_(std::move(intra)), shm_(std::move(shm)) {}

  void write(const void* data, std::size_t size) override {
    intra_->write(data, size);
    shm_->write(data, size);
  }

  std::string_view channel() const override { return intra_->channel(); }

 private:
  std::unique_ptr<WriterBase> intra_;
  std::unique_ptr<WriterBase> shm_;
};

}  // namespace tianshu::transport
