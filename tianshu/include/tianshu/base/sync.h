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

// Synchronization primitives: BlockingCounter and Notification.
//
// Design (per L4-PRIM-6):
//   - BlockingCounter: count down from N, block until zero (barrier pattern)
//   - Notification: one-shot signal, wait for notify (event pattern)

#pragma once

#include <condition_variable>
#include <mutex>

namespace tianshu::base {

class BlockingCounter {
 public:
  explicit BlockingCounter(int initial) : count_(initial) {}

  BlockingCounter(const BlockingCounter&) = delete;
  BlockingCounter& operator=(const BlockingCounter&) = delete;

  void increment() {
    const std::scoped_lock lock(mutex_);
    ++count_;
  }

  void decrement() {
    const std::scoped_lock lock(mutex_);
    if (--count_ <= 0) {
      cv_.notify_all();
    }
  }

  void wait() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return count_ <= 0; });
  }

  int count() const {
    const std::scoped_lock lock(mutex_);
    return count_;
  }

 private:
  int count_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
};

class Notification {
 public:
  Notification() = default;

  Notification(const Notification&) = delete;
  Notification& operator=(const Notification&) = delete;

  void notify() {
    {
      const std::scoped_lock lock(mutex_);
      notified_ = true;
    }
    cv_.notify_all();
  }

  bool has_been_notified() const {
    const std::scoped_lock lock(mutex_);
    return notified_;
  }

  void wait() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return notified_; });
  }

 private:
  bool notified_{false};
  mutable std::mutex mutex_;
  std::condition_variable cv_;
};

}  // namespace tianshu::base
