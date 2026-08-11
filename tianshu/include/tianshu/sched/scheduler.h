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

// Callback-based task scheduler (no coroutines, per ADR-0019).
//
// Design (per L4-SCHED-1/2/3, ADR-0019):
//   - Task = named function + priority + state machine
//   - Scheduler picks highest-priority READY task, calls fn(), picks next
//   - Multi-threaded: N worker threads pulling from shared priority queue
//   - Priority/timing guaranteed by OS thread scheduling (ADR-0019)
//   - No preemption within a single Proc() (runs to completion)

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tianshu::sched {
enum class TaskState : uint8_t {
  kReady,
  kRunning,
  kWaiting,
  kDone,
};

struct Task {
  std::string name;
  int priority{0};
  TaskState state{TaskState::kReady};
  std::function<void()> fn;
  uint64_t run_count{0};

  Task() = default;
  Task(std::string n, int prio, std::function<void()> f)
      : name(std::move(n)), priority(prio), fn(std::move(f)) {}

  bool operator<(const Task& other) const { return priority < other.priority; }
};
class Scheduler {
 public:
  Scheduler() = default;

  explicit Scheduler(std::size_t num_threads) : num_threads_(num_threads == 0 ? 1 : num_threads) {}

  ~Scheduler() { shutdown(); }

  Scheduler(const Scheduler&) = delete;
  Scheduler& operator=(const Scheduler&) = delete;

  void add_task(std::string name, int priority, std::function<void()> fn) {
    const std::scoped_lock lock(mutex_);
    auto& task = tasks_[name];
    task = Task(std::move(name), priority, std::move(fn));
    ready_queue_.push(&task);
  }

  void mark_ready(std::string_view name) {
    const std::scoped_lock lock(mutex_);
    auto it = tasks_.find(std::string(name));
    if (it != tasks_.end() && it->second.state == TaskState::kWaiting) {
      it->second.state = TaskState::kReady;
      ready_queue_.push(&it->second);
      cv_.notify_one();
    }
  }

  void mark_waiting(std::string_view name) {
    const std::scoped_lock lock(mutex_);
    auto it = tasks_.find(std::string(name));
    if (it != tasks_.end()) {
      it->second.state = TaskState::kWaiting;
    }
  }

  void start() {
    shutdown_.store(false, std::memory_order_release);
    workers_.reserve(num_threads_);
    for (std::size_t i = 0; i < num_threads_; ++i) {
      workers_.emplace_back([this] { worker_loop(); });
    }
  }

  void shutdown() {
    shutdown_.store(true, std::memory_order_release);
    cv_.notify_all();
    for (auto& w : workers_) {
      if (w.joinable()) {
        w.join();
      }
    }
    workers_.clear();
  }

  std::size_t task_count() const {
    const std::scoped_lock lock(mutex_);
    return tasks_.size();
  }

 private:
  void worker_loop() {
    while (true) {
      Task* task = nullptr;
      {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] {
          return shutdown_.load(std::memory_order_acquire) || !ready_queue_.empty();
        });

        if (shutdown_.load(std::memory_order_acquire)) {
          return;
        }

        if (!ready_queue_.empty()) {
          task = ready_queue_.top();
          ready_queue_.pop();
        }
      }

      if (task != nullptr && task->fn) {
        task->state = TaskState::kRunning;
        task->fn();
        ++task->run_count;

        const std::scoped_lock lock(mutex_);
        if (task->state == TaskState::kRunning) {
          task->state = TaskState::kReady;
          ready_queue_.push(task);
        }
      }
    }
  }

  struct TaskPtrCompare {
    bool operator()(const Task* a, const Task* b) const { return a->priority < b->priority; }
  };

  std::size_t num_threads_{1};
  std::unordered_map<std::string, Task> tasks_;
  std::priority_queue<Task*, std::vector<Task*>, TaskPtrCompare> ready_queue_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::atomic<bool> shutdown_{false};
  std::vector<std::thread> workers_;
};

}  // namespace tianshu::sched
