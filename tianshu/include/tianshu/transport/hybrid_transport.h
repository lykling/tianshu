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
// Design (per L4-TRANS-19, ADR-0010):
//   - kIntra: same process (zero-copy, default)
//   - kShm: same machine, cross process
//   - kAuto: service-discovery based selection (L4-TRANS-21, future)

#pragma once

#include <memory>

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

  std::unique_ptr<WriterBase> create_writer(const ChannelConfig& cfg) override {
    return active_backend(cfg)->create_writer(cfg);
  }

  std::unique_ptr<ReaderBase> create_reader(const ChannelConfig& cfg) override {
    return active_backend(cfg)->create_reader(cfg);
  }

 private:
  TransportBackend* active_backend(const ChannelConfig& cfg);

  TransportMode mode_;
  std::unique_ptr<TransportBackend> intra_;
  std::unique_ptr<TransportBackend> shm_;
};

}  // namespace tianshu::transport
