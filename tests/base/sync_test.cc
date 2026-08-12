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

// Unit tests for BlockingCounter and Notification (L4-PRIM-6).

#include "tianshu/base/sync.h"

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

namespace {

// ---------------------------------------------------------------------------
// BlockingCounter
// ---------------------------------------------------------------------------

TEST(BlockingCounterTest, InitiallyNotZero) {
  const tianshu::base::BlockingCounter counter(3);
  EXPECT_EQ(counter.count(), 3);
}

TEST(BlockingCounterTest, Increment) {
  tianshu::base::BlockingCounter counter(1);
  counter.increment();
  EXPECT_EQ(counter.count(), 2);
}

TEST(BlockingCounterTest, IncrementMultiple) {
  tianshu::base::BlockingCounter counter(0);
  counter.increment();
  counter.increment();
  counter.increment();
  EXPECT_EQ(counter.count(), 3);
}

TEST(BlockingCounterTest, DecrementToZeroNotifies) {
  tianshu::base::BlockingCounter counter(2);
  counter.decrement();
  EXPECT_EQ(counter.count(), 1);
  counter.decrement();
  EXPECT_EQ(counter.count(), 0);
}

TEST(BlockingCounterTest, WaitReturnsImmediatelyWhenAlreadyZero) {
  tianshu::base::BlockingCounter counter(0);
  // Should return immediately, not block.
  counter.wait();
}

TEST(BlockingCounterTest, WaitReturnsWhenZero) {
  tianshu::base::BlockingCounter counter(2);
  bool done = false;

  std::thread waiter([&]() {
    counter.wait();
    done = true;
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_FALSE(done);

  counter.decrement();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_FALSE(done);

  counter.decrement();
  waiter.join();
  EXPECT_TRUE(done);
}

TEST(BlockingCounterTest, IncrementAfterZeroUnblocks) {
  tianshu::base::BlockingCounter counter(1);

  std::thread waiter([&]() { counter.wait(); });

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  counter.decrement();
  waiter.join();

  // After wait returns, increment and decrement again.
  counter.increment();
  EXPECT_EQ(counter.count(), 1);
  counter.decrement();
  EXPECT_EQ(counter.count(), 0);
}

TEST(BlockingCounterTest, ConcurrentDecrement) {
  constexpr int kThreads = 4;
  tianshu::base::BlockingCounter counter(kThreads);

  std::thread threads[kThreads];
  for (int i = 0; i < kThreads; ++i) {
    threads[i] = std::thread([&]() { counter.decrement(); });
  }

  counter.wait();
  for (auto& t : threads) {
    t.join();
  }
  EXPECT_EQ(counter.count(), 0);
}

// ---------------------------------------------------------------------------
// Notification
// ---------------------------------------------------------------------------

TEST(NotificationTest, InitiallyNotNotified) {
  const tianshu::base::Notification n;
  EXPECT_FALSE(n.has_been_notified());
}

TEST(NotificationTest, NotifySetsFlag) {
  tianshu::base::Notification n;
  n.notify();
  EXPECT_TRUE(n.has_been_notified());
}

TEST(NotificationTest, WaitReturnsAfterNotify) {
  tianshu::base::Notification n;
  bool done = false;

  std::thread waiter([&]() {
    n.wait();
    done = true;
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_FALSE(done);

  n.notify();
  waiter.join();
  EXPECT_TRUE(done);
}

TEST(NotificationTest, DoubleNotifyIsNoop) {
  tianshu::base::Notification n;
  n.notify();
  n.notify();
  EXPECT_TRUE(n.has_been_notified());
}

TEST(NotificationTest, WaitReturnsImmediatelyIfAlreadyNotified) {
  tianshu::base::Notification n;
  n.notify();
  n.wait();
}

TEST(NotificationTest, MultipleWaitersAllNotified) {
  tianshu::base::Notification n;
  std::atomic<int> ready_count{0};

  std::thread w1([&]() {
    n.wait();
    ready_count.fetch_add(1);
  });
  std::thread w2([&]() {
    n.wait();
    ready_count.fetch_add(1);
  });
  std::thread w3([&]() {
    n.wait();
    ready_count.fetch_add(1);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_EQ(ready_count.load(), 0);

  n.notify();
  w1.join();
  w2.join();
  w3.join();

  EXPECT_EQ(ready_count.load(), 3);
}

}  // namespace
