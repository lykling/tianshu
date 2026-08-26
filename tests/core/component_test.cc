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

// Unit tests for Component framework (L4-COMP-1/2/3/10).

#include "tianshu/core/component.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

#include "tianshu/core/data_dispatcher.h"
#include "tianshu/core/data_visitor.h"
#include "tianshu/core/node.h"

namespace {

struct Raw {
  double value;
};

struct Fused {
  double doubled;
};

class DoublerComponent : public tianshu::core::Component<Raw, Fused> {
 public:
  explicit DoublerComponent(std::string name) : Component(std::move(name)) {}
  std::atomic<int> proc_calls{0};
  double last_out{0};

 protected:
  void proc(const Raw& msg) override {
    proc_calls.fetch_add(1);
    last_out = msg.value * 2;
    publish(Fused{.doubled = last_out});
  }

  [[nodiscard]] std::string_view out_channel() const override { return "/comp/fused"; }
};

TEST(ComponentTest, SingleInputProcAndPublish) {
  tianshu::core::Node node;
  DoublerComponent comp("doubler");
  ASSERT_TRUE(comp.start(&node, "/comp/raw"));

  auto reader = node.create_typed_reader<Fused>("/comp/fused");

  auto& dispatcher = tianshu::core::DataDispatcher::instance();
  const Raw raw{.value = 21};
  dispatcher.dispatch(tianshu::core::channel_id_for("/comp/raw"), &raw, sizeof(raw));

  EXPECT_EQ(comp.proc_calls.load(), 1);
  EXPECT_DOUBLE_EQ(comp.last_out, 42);
  ASSERT_NE(reader->try_fetch(), nullptr);
  EXPECT_DOUBLE_EQ(reader->try_fetch()->doubled, 42);
}

TEST(ComponentTest, NameAndLifecycleDefaults) {
  DoublerComponent comp("named");
  EXPECT_EQ(comp.name(), "named");
  EXPECT_TRUE(comp.init());
  comp.shutdown();
}

class SumComponent : public tianshu::core::TwoInputComponent<Raw, Raw> {
 public:
  explicit SumComponent(std::string name) : TwoInputComponent(std::move(name)) {}
  std::atomic<int> proc_calls{0};
  double last_sum{0};

 protected:
  void proc(const Raw& a, const Raw& b) override {
    proc_calls.fetch_add(1);
    last_sum = a.value + b.value;
  }

  [[nodiscard]] std::string_view out_channel() const override { return "/comp/sum_out"; }
};

TEST(ComponentTest, TwoInputAllLatestFusion) {
  tianshu::core::Node node;
  SumComponent comp("summer");
  ASSERT_TRUE(comp.start(&node, "/comp/sum_a", "/comp/sum_b"));

  auto& dispatcher = tianshu::core::DataDispatcher::instance();
  const Raw a{.value = 1.5};
  dispatcher.dispatch(tianshu::core::channel_id_for("/comp/sum_a"), &a, sizeof(a));
  EXPECT_EQ(comp.proc_calls.load(), 0);

  const Raw b{.value = 2.5};
  dispatcher.dispatch(tianshu::core::channel_id_for("/comp/sum_b"), &b, sizeof(b));
  EXPECT_EQ(comp.proc_calls.load(), 1);
  EXPECT_DOUBLE_EQ(comp.last_sum, 4.0);
}

class TickComponent : public tianshu::core::TimerComponent {
 public:
  explicit TickComponent(std::string name) : TimerComponent(std::move(name)) {}
  std::atomic<int> ticks{0};

 protected:
  void proc() override { ticks.fetch_add(1); }
};

TEST(TimerComponentTest, PeriodicProc) {
  TickComponent ticker("ticker");
  ASSERT_TRUE(ticker.start(std::chrono::milliseconds(10)));

  std::this_thread::sleep_for(std::chrono::milliseconds(105));
  ticker.stop();

  EXPECT_GE(ticker.ticks.load(), 5);
  EXPECT_LE(ticker.ticks.load(), 15);
}

TEST(TimerComponentTest, RejectsNonPositiveInterval) {
  TickComponent ticker("bad");
  EXPECT_FALSE(ticker.start(std::chrono::milliseconds(0)));
}

class FactoryProbeComponent : public tianshu::core::Component<Raw> {
 public:
  explicit FactoryProbeComponent(std::string name) : Component(std::move(name)) {}

 protected:
  void proc(const Raw& /*msg*/) override {}
  [[nodiscard]] std::string_view out_channel() const override { return "/comp/probe"; }
};

TEST(ComponentFactoryTest, RegisterAndCreate) {
  auto& factory = tianshu::core::ComponentFactory::instance();
  auto comp = factory.create("factory_probe", "inst1");
  ASSERT_NE(comp, nullptr);
  EXPECT_EQ(comp->name(), "inst1");
  EXPECT_EQ(dynamic_cast<FactoryProbeComponent*>(comp.get()) != nullptr, true);
}

TEST(ComponentFactoryTest, UnknownTypeReturnsNull) {
  auto& factory = tianshu::core::ComponentFactory::instance();
  EXPECT_EQ(factory.create("no_such_type", "x"), nullptr);
}

}  // namespace

TIANSHU_REGISTER_COMPONENT(FactoryProbeComponent, "factory_probe")
