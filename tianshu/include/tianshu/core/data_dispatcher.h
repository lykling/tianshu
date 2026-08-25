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

// DataDispatcher: routes incoming channel data to registered buffers.
//
// Design (per L4-CORE-6, cyber-equivalent):
//   - Singleton mapping channel_id -> set of CacheBuffer sinks
//   - Writer<T>::write() pushes through the dispatcher after transport send
//   - Each DataVisitor registers its per-channel CacheBuffer on construction
//   - After filling a buffer, the visitor's notify callback fires

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "tianshu/base/cache_buffer.h"
#include "tianshu/base/spin_lock.h"

namespace tianshu::core {

using ChannelId = std::uint64_t;
using NotifyFunc = std::function<void()>;

class DataDispatcher {
 public:
  static DataDispatcher& instance();

  DataDispatcher(const DataDispatcher&) = delete;
  DataDispatcher& operator=(const DataDispatcher&) = delete;

  // Register a sink for a channel. Each visitor owns its buffer lifetime;
  // the dispatcher holds only raw pointers, valid while the visitor lives.
  void add_buffer(ChannelId channel_id, base::CacheBufferBase* buffer, NotifyFunc notify,
                  const void* owner);

  // Drop every sink registered by `owner` (visitor teardown).
  void remove_owner(const void* owner);

  // Fan out one message: fills every registered buffer, then notifies.
  void dispatch(ChannelId channel_id, const void* data, std::size_t size);

 private:
  DataDispatcher() = default;

  struct Sink {
    base::CacheBufferBase* buffer;
    NotifyFunc notify;
    const void* owner;
  };

  std::unordered_map<ChannelId, std::vector<Sink>> sinks_;
  base::SpinLock lock_;
};

}  // namespace tianshu::core
