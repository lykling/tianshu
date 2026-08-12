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

#include "tianshu/transport/intra_backend.h"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace tianshu::transport::intra {

IntraChannelRegistry& IntraChannelRegistry::instance() {
  static IntraChannelRegistry registry;
  return registry;
}

std::shared_ptr<IntraWriter> IntraChannelRegistry::get_or_create_writer(std::string_view channel) {
  const std::scoped_lock lock(mutex_);
  const std::string key(channel);
  auto it = writers_.find(key);
  if (it != writers_.end()) {
    return it->second;
  }
  auto writer = std::make_shared<IntraWriter>(key);
  writers_[key] = writer;
  return writer;
}

}  // namespace tianshu::transport::intra
