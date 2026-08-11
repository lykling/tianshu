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

// Unit tests for Scheduler (L4-SCHED-1/2/3).

#include "tianshu/sched/scheduler.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <gtest/gtest.h>

namespace {

TEST(SchedulerTest, AddTaskAndRun) {
  tianshu::sched::Scheduler sched(1);
  std::atomic<int> counter{0};

  sched.add_task("task_a", 1, [&]() { counter.fetch_add(1); });
  sched.start();

  while (counter.load() == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  sched.shutdown();
  EXPECT_GE(counter.load(), 1);
}

TEST(SchedulerTest, PriorityFirstExecution) {
  tianshu::sched::Scheduler sched(1);
  std::atomic<int> first_runner{0};

  sched.add_task("low", 1, [&]() {
    int expected = 0;
    first_runner.compare_exchange_strong(expected, 1);
  });
  sched.add_task("high", 10, [&]() {
    int expected = 0;
    first_runner.compare_exchange_strong(expected, 2);
  });

  sched.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  sched.shutdown();

  EXPECT_EQ(first_runner.load(), 2);
}

TEST(SchedulerTest, MultipleWorkers) {
  tianshu::sched::Scheduler sched(4);
  std::atomic<int> counter{0};

  for (int i = 0; i < 10; ++i) {
    sched.add_task("task_" + std::to_string(i), 1, [&]() { counter.fetch_add(1); });
  }

  sched.start();
  while (counter.load() < 10) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  sched.shutdown();

  EXPECT_GE(counter.load(), 10);
}

TEST(SchedulerTest, MarkWaitingAndReady) {
  tianshu::sched::Scheduler sched(1);
  std::atomic<int> run_count{0};

  sched.add_task("waiter", 5, [&]() {
    run_count.fetch_add(1);
    sched.mark_waiting("waiter");
  });

  sched.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(run_count.load(), 1);

  sched.mark_ready("waiter");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  sched.shutdown();

  EXPECT_GE(run_count.load(), 2);
}

TEST(SchedulerTest, ShutdownStopsWorkers) {
  tianshu::sched::Scheduler sched(2);
  sched.add_task("loop", 1, []() { std::this_thread::sleep_for(std::chrono::milliseconds(1)); });

  sched.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  sched.shutdown();

  SUCCEED();
}

TEST(SchedulerTest, TaskCount) {
  tianshu::sched::Scheduler sched(1);
  sched.add_task("a", 1, []() {});
  sched.add_task("b", 2, []() {});
  sched.add_task("c", 3, []() {});
  EXPECT_EQ(sched.task_count(), 3U);
}

}  // namespace
