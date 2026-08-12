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

#include "tianshu/transport/transport_backend.h"

#include <functional>
#include <memory>

namespace tianshu::transport {

WriterBase::~WriterBase() = default;              // LCOV_EXCL_LINE
ReaderBase::~ReaderBase() = default;              // LCOV_EXCL_LINE
TransportBackend::~TransportBackend() = default;  // LCOV_EXCL_LINE

TransportRegistry& TransportRegistry::instance() {
  static TransportRegistry registry;
  return registry;
}

void TransportRegistry::register_backend(
    BackendType type, const std::function<std::unique_ptr<TransportBackend>()>& factory) {
  backends_[type] = factory();
}

TransportBackend* TransportRegistry::get(BackendType type) {
  auto it = backends_.find(type);
  if (it == backends_.end()) {
    return nullptr;
  }
  return it->second.get();
}

}  // namespace tianshu::transport
