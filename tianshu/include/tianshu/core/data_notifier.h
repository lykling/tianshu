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

// DataNotifier: wakes the scheduler when a visitor's data is ready.
//
// Design (per L4-CORE-7, cyber-equivalent):
//   - Singleton mapping channel_id -> notifier callbacks
//   - DataDispatcher invokes notify after filling buffers
//   - Notifier re-enqueues the owning task into the Scheduler

#pragma once

#include <cstddef>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "tianshu/core/data_dispatcher.h"

namespace tianshu::core {

class DataNotifier {
 public:
  static DataNotifier& instance();

  DataNotifier(const DataNotifier&) = delete;
  DataNotifier& operator=(const DataNotifier&) = delete;

  void add_notifier(ChannelId channel_id, NotifyFunc notify, const void* owner);

  // Drop callbacks registered by `owner` (visitor teardown).
  void remove_owner(const void* owner);

  // Fire every notifier for the channel. Returns how many fired.
  std::size_t notify(ChannelId channel_id);

 private:
  DataNotifier() = default;

  struct Entry {
    NotifyFunc notify;
    const void* owner;
  };

  std::unordered_map<ChannelId, std::vector<Entry>> entries_;
  std::mutex mutex_;
};

}  // namespace tianshu::core
