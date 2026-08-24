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

namespace tianshu::transport {

// Phase 1: kAuto falls back to INTRA. L4-TRANS-21 (service discovery based
// process detection) will replace this.
TransportBackend* HybridTransport::active_backend(const ChannelConfig& cfg) {
  switch (mode_) {
    case TransportMode::kShm:
      return shm_.get();
    case TransportMode::kIntra:
    case TransportMode::kAuto:
    default:
      return intra_.get();
  }
}

}  // namespace tianshu::transport
