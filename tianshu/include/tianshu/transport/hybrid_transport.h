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

// Hybrid transport: auto-selects the best backend per channel.
//
// Design (per L4-TRANS-19, ADR-0010):
//   - Same process → INTRA (zero-copy)
//   - Same machine (future) → SHM
//   - Cross-machine (future) → Zenoh
//   - Phase 1: INTRA only (SHM/Zenoh added in later commits)

#pragma once

#include <memory>

#include "tianshu/transport/intra_backend.h"
#include "tianshu/transport/transport_backend.h"

namespace tianshu::transport {

class HybridTransport : public TransportBackend {
 public:
  HybridTransport() : intra_(std::make_unique<intra::IntraBackend>()) {}

  BackendType type() const override { return BackendType::kIntra; }
  bool supports_zero_copy() const override { return true; }
  bool supports_remote() const override { return false; }

  std::unique_ptr<WriterBase> create_writer(const ChannelConfig& cfg) override {
    // Phase 1: always INTRA. Phase 2: add SHM/Zenoh selection.
    return intra_->create_writer(cfg);
  }

  std::unique_ptr<ReaderBase> create_reader(const ChannelConfig& cfg) override {
    return intra_->create_reader(cfg);
  }

 private:
  std::unique_ptr<TransportBackend> intra_;
};

}  // namespace tianshu::transport
