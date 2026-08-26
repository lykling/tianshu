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

#include "tianshu/core/data_notifier.h"

#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

#include "tianshu/core/data_dispatcher.h"

namespace tianshu::core {

DataNotifier& DataNotifier::instance() {
  static DataNotifier notifier;
  return notifier;
}

void DataNotifier::add_notifier(ChannelId channel_id, NotifyFunc notify, const void* owner) {
  const std::scoped_lock lock(mutex_);
  entries_[channel_id].push_back(Entry{.notify = std::move(notify), .owner = owner});
}

void DataNotifier::remove_owner(const void* owner) {
  const std::scoped_lock lock(mutex_);
  for (auto it = entries_.begin(); it != entries_.end();) {
    auto& vec = it->second;
    std::erase_if(vec, [owner](const Entry& e) { return e.owner == owner; });
    if (vec.empty()) {
      it = entries_.erase(it);
    } else {
      ++it;
    }
  }
}

std::size_t DataNotifier::notify(ChannelId channel_id) {
  std::vector<NotifyFunc> to_fire;
  {
    const std::scoped_lock lock(mutex_);
    const auto it = entries_.find(channel_id);
    if (it == entries_.end()) {
      return 0;
    }
    to_fire.reserve(it->second.size());
    for (const Entry& e : it->second) {
      if (e.notify) {
        to_fire.push_back(e.notify);
      }
    }
  }
  for (const NotifyFunc& fn : to_fire) {
    fn();
  }
  return to_fire.size();
}

}  // namespace tianshu::core
