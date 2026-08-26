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

#include "tianshu/core/component.h"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace tianshu::core {

ComponentFactory& ComponentFactory::instance() {
  static ComponentFactory factory;
  return factory;
}

void ComponentFactory::register_component(std::string_view type_name, Creator creator) {
  const std::scoped_lock lock(mutex_);
  creators_.emplace_back(std::string(type_name), std::move(creator));
}

std::unique_ptr<ComponentBase> ComponentFactory::create(std::string_view type_name,
                                                        std::string name) const {
  const std::scoped_lock lock(mutex_);
  for (const auto& [key, creator] : creators_) {
    if (key == type_name) {
      return creator(std::move(name));
    }
  }
  return nullptr;
}

}  // namespace tianshu::core
