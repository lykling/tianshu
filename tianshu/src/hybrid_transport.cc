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

#include "tianshu/transport/hybrid_transport.h"

#include <memory>

#include "tianshu/transport/intra_backend.h"
#include "tianshu/transport/transport_backend.h"

namespace tianshu::transport {

// kAuto semantics per ADR-0023 (no discovery service in v0): writers
// dual-publish; readers prefer INTRA when this process hosts a real
// writer on the channel, else attach SHM.
TransportBackend* HybridTransport::active_backend([[maybe_unused]] const ChannelConfig& cfg) {
  switch (mode_) {
    case TransportMode::kShm:
      return shm_.get();
    case TransportMode::kIntra:
    case TransportMode::kAuto:
    default:
      return intra_.get();
  }
}

std::unique_ptr<WriterBase> HybridTransport::create_writer(const ChannelConfig& cfg) {
  if (mode_ == TransportMode::kAuto) {
    return std::make_unique<AutoWriter>(intra_->create_writer(cfg), shm_->create_writer(cfg));
  }
  return active_backend(cfg)->create_writer(cfg);
}

std::unique_ptr<ReaderBase> HybridTransport::create_reader(const ChannelConfig& cfg) {
  if (mode_ == TransportMode::kAuto) {
    if (intra::IntraChannelRegistry::instance().has_writer(cfg.channel_name)) {
      return intra_->create_reader(cfg);
    }
    return shm_->create_reader(cfg);
  }
  return active_backend(cfg)->create_reader(cfg);
}

}  // namespace tianshu::transport
