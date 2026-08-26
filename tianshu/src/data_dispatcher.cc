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

#include "tianshu/core/data_dispatcher.h"

#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

#include "tianshu/base/cache_buffer.h"

namespace tianshu::core {

DataDispatcher& DataDispatcher::instance() {
  static DataDispatcher dispatcher;
  return dispatcher;
}

void DataDispatcher::add_buffer(ChannelId channel_id, base::CacheBufferBase* buffer,
                                NotifyFunc notify, const void* owner) {
  const std::scoped_lock lock(lock_);
  sinks_[channel_id].push_back(Sink{.buffer = buffer, .notify = std::move(notify), .owner = owner});
}

void DataDispatcher::remove_owner(const void* owner) {
  const std::scoped_lock lock(lock_);
  for (auto it = sinks_.begin(); it != sinks_.end();) {
    auto& vec = it->second;
    std::erase_if(vec, [owner](const Sink& s) { return s.owner == owner; });
    if (vec.empty()) {
      it = sinks_.erase(it);
    } else {
      ++it;
    }
  }
}

void DataDispatcher::dispatch(ChannelId channel_id, const void* data, std::size_t size) {
  std::vector<NotifyFunc> to_notify;
  {
    const std::scoped_lock lock(lock_);
    const auto it = sinks_.find(channel_id);
    if (it == sinks_.end()) {
      return;
    }
    to_notify.reserve(it->second.size());
    for (Sink& s : it->second) {
      s.buffer->fill_bytes(data, size);
      if (s.notify) {
        to_notify.push_back(s.notify);
      }
    }
  }
  // Callbacks run outside the lock: they may register/unregister sinks.
  for (const NotifyFunc& fn : to_notify) {
    fn();
  }
}

}  // namespace tianshu::core
