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

// Component framework: user operators wired into the dataflow graph.
//
// Design (per L4-COMP-1/2/3/10, cyber-equivalent):
//   - ComponentBase: Init/Shutdown lifecycle + name
//   - Component<M0..M3>: typed inputs, AllLatest fusion via DataVisitor;
//     user overrides Proc(const M0&, ...) which runs whenever all inputs
//     have fresh data
//   - TimerComponent: Proc on a fixed interval, no inputs
//   - ComponentFactory + TIANSHU_REGISTER_COMPONENT: name -> creator map,
//     mainboard loads DAG configs through it

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "tianshu/core/data_visitor.h"
#include "tianshu/core/node.h"
#include "tianshu/core/typed_writer.h"

namespace tianshu::core {

class ComponentBase {
 public:
  explicit ComponentBase(std::string name) : name_(std::move(name)) {}
  virtual ~ComponentBase() = default;

  ComponentBase(const ComponentBase&) = delete;
  ComponentBase& operator=(const ComponentBase&) = delete;

  virtual bool init() { return true; }
  virtual void shutdown() {}

  [[nodiscard]] const std::string& name() const { return name_; }

 private:
  std::string name_;
};

// ---------------------------------------------------------------------------
// Component: typed inputs + AllLatest fusion
//   Component<In, Out>            single input
//   Component<In0, In1, Out>      two inputs
// Output type defaults to the first input type.
// ---------------------------------------------------------------------------

template <typename M0, typename TOut = M0>
class Component : public ComponentBase {
 public:
  explicit Component(std::string name) : ComponentBase(std::move(name)) {}

  bool start(Node* node, std::string_view ch0) {
    node_ = node;
    writer_ = node_->create_typed_writer<TOut>(out_channel());
    visitor_ = std::make_unique<DataVisitor<M0>>(ch0, queue_depth(), [this] { run_proc(); });
    return true;
  }

 protected:
  virtual void proc(const M0& msg) = 0;

  [[nodiscard]] virtual std::string_view out_channel() const = 0;
  [[nodiscard]] virtual std::size_t queue_depth() const { return 16; }

  void publish(const TOut& msg) { writer_->write(msg); }

 private:
  void run_proc() {
    while (M0* msg = visitor_->try_fetch_0()) {
      proc(*msg);
    }
  }

  Node* node_{nullptr};
  std::unique_ptr<Writer<TOut>> writer_;
  std::unique_ptr<DataVisitor<M0>> visitor_;
};

template <typename M0, typename M1, typename TOut = M0>
class TwoInputComponent : public ComponentBase {
 public:
  explicit TwoInputComponent(std::string name) : ComponentBase(std::move(name)) {}

  bool start(Node* node, std::string_view ch0, std::string_view ch1) {
    node_ = node;
    writer_ = node_->create_typed_writer<TOut>(out_channel());
    visitor_ =
        std::make_unique<DataVisitor<M0, M1>>(ch0, ch1, queue_depth(), [this] { run_proc(); });
    return true;
  }

 protected:
  virtual void proc(const M0& msg0, const M1& msg1) = 0;

  [[nodiscard]] virtual std::string_view out_channel() const = 0;
  [[nodiscard]] virtual std::size_t queue_depth() const { return 16; }

  void publish(const TOut& msg) { writer_->write(msg); }

 private:
  void run_proc() {
    M0* msg0 = nullptr;
    M1* msg1 = nullptr;
    while ((msg0 = visitor_->try_fetch_0()) != nullptr &&
           (msg1 = visitor_->try_fetch_1()) != nullptr) {
      proc(*msg0, *msg1);
    }
  }

  Node* node_{nullptr};
  std::unique_ptr<Writer<TOut>> writer_;
  std::unique_ptr<DataVisitor<M0, M1>> visitor_;
};

// ---------------------------------------------------------------------------
// TimerComponent: periodic Proc without inputs
// ---------------------------------------------------------------------------

class TimerComponent : public ComponentBase {
 public:
  explicit TimerComponent(std::string name) : ComponentBase(std::move(name)) {}
  ~TimerComponent() override { stop(); }

  TimerComponent(const TimerComponent&) = delete;
  TimerComponent& operator=(const TimerComponent&) = delete;

  bool start(std::chrono::milliseconds interval) {
    if (interval.count() <= 0) {
      return false;
    }
    stop_.store(false, std::memory_order_release);
    thread_ = std::thread([this, interval] {
      auto next = std::chrono::steady_clock::now() + interval;
      while (!stop_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_until(next);
        if (stop_.load(std::memory_order_acquire)) {
          return;
        }
        proc();
        next += interval;
      }
    });
    return true;
  }

  void stop() {
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
      thread_.join();
    }
  }

 protected:
  virtual void proc() = 0;

 private:
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

// ---------------------------------------------------------------------------
// ComponentFactory (L4-COMP-10)
// ---------------------------------------------------------------------------

class ComponentFactory {
 public:
  using Creator = std::function<std::unique_ptr<ComponentBase>(std::string name)>;

  static ComponentFactory& instance();

  void register_component(std::string_view type_name, Creator creator);
  [[nodiscard]] std::unique_ptr<ComponentBase> create(std::string_view type_name,
                                                      std::string name) const;

 private:
  ComponentFactory() = default;
  std::vector<std::pair<std::string, Creator>> creators_;
  mutable std::mutex mutex_;
};

}  // namespace tianshu::core

#define TIANSHU_REGISTER_COMPONENT(TypeClass, TypeNameStr)            \
  namespace {                                                         \
  /* NOLINTBEGIN(bugprone-throwing-static-initialization) */          \
  /* NOLINTBEGIN(readability-identifier-naming) */                    \
  const bool TypeClass##_registered [[maybe_unused]] = [] {           \
    ::tianshu::core::ComponentFactory::instance().register_component( \
        TypeNameStr, [](std::string name) {                           \
          return std::unique_ptr<::tianshu::core::ComponentBase>(     \
              std::make_unique<TypeClass>(std::move(name)));          \
        });                                                           \
    return true;                                                      \
  }();                                                                \
  /* NOLINTEND(readability-identifier-naming) */                      \
  /* NOLINTEND(bugprone-throwing-static-initialization) */            \
  }
