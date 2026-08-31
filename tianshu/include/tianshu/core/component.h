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

#include "tianshu/core/data_dispatcher.h"
#include "tianshu/core/data_visitor.h"
#include "tianshu/core/node.h"
#include "tianshu/core/typed_writer.h"
#include "tianshu/transport/transport_backend.h"

namespace tianshu::core {

class ComponentBase {
 public:
  explicit ComponentBase(std::string name) : name_(std::move(name)) {}
  virtual ~ComponentBase() = default;

  ComponentBase(const ComponentBase&) = delete;
  ComponentBase& operator=(const ComponentBase&) = delete;

  virtual bool init() { return true; }
  virtual void shutdown() {}

  // Generic entry used by the launcher: wires inputs/output channels and
  // periodicity from a DagConfig entry. Implemented by the templates below,
  // not by user classes.
  virtual bool launch(Node& node, const std::vector<std::string>& input_channels,
                      std::chrono::milliseconds interval) = 0;

  // Output-channel injection for bridge wiring (ADR-0025): flow-declared
  // channels override the class-declared out_channel(). Default no-op;
  // the templates below honor it.
  virtual void set_out_channel_override(const std::string& channel) { static_cast<void>(channel); }

  // Stops self-driven execution (timer threads) and joins them, so no
  // callback is in flight when the owner tears the graph down. Default
  // no-op (input-driven components only run inside a dispatch).
  virtual void quiesce() {}

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
    if (!effective_out_channel().empty()) {
      writer_ = node_->create_typed_writer<TOut>(effective_out_channel());
    }
    bridge_input(ch0);
    visitor_ = std::make_unique<DataVisitor<M0>>(ch0, queue_depth(), [this] { run_proc(); });
    return true;
  }

  bool launch(Node& node, const std::vector<std::string>& input_channels,
              std::chrono::milliseconds /*interval*/) override {
    if (input_channels.empty()) {
      return false;
    }
    return start(&node, input_channels[0]);
  }

 protected:
  virtual void proc(const M0& msg) = 0;

  [[nodiscard]] virtual std::string_view out_channel() const = 0;
  [[nodiscard]] virtual std::size_t queue_depth() const { return 16; }

  void publish(const TOut& msg) { writer_->write(msg); }

 public:
  void set_out_channel_override(const std::string& channel) override {
    out_channel_override_ = channel;
  }

 private:
  void run_proc() {
    while (M0* msg = visitor_->try_fetch_0()) {
      proc(*msg);
    }
  }

  // Transport reader forwarding into the DataDispatcher so typed writers on
  // the same channel reach this component's visitor.
  void bridge_input(std::string_view channel) {
    const ChannelId id = channel_id_for(channel);
    auto reader = node_->create_reader(channel);
    reader->set_callback([id](const transport::Message& msg) {
      DataDispatcher::instance().dispatch(id, msg.data, msg.size);
    });
    input_readers_.push_back(std::move(reader));
  }

  Node* node_{nullptr};
  std::unique_ptr<Writer<TOut>> writer_;
  std::unique_ptr<DataVisitor<M0>> visitor_;
  std::vector<std::unique_ptr<transport::ReaderBase>> input_readers_;
  std::string out_channel_override_;

  [[nodiscard]] std::string effective_out_channel() const {
    return out_channel_override_.empty() ? std::string(out_channel()) : out_channel_override_;
  }
};

template <typename M0, typename M1, typename TOut = M0>
class TwoInputComponent : public ComponentBase {
 public:
  explicit TwoInputComponent(std::string name) : ComponentBase(std::move(name)) {}

  bool start(Node* node, std::string_view ch0, std::string_view ch1) {
    node_ = node;
    if (!effective_out_channel().empty()) {
      writer_ = node_->create_typed_writer<TOut>(effective_out_channel());
    }
    bridge_input(ch0);
    bridge_input(ch1);
    visitor_ =
        std::make_unique<DataVisitor<M0, M1>>(ch0, ch1, queue_depth(), [this] { run_proc(); });
    return true;
  }

  bool launch(Node& node, const std::vector<std::string>& input_channels,
              std::chrono::milliseconds /*interval*/) override {
    if (input_channels.size() < 2) {
      return false;
    }
    return start(&node, input_channels[0], input_channels[1]);
  }

 protected:
  virtual void proc(const M0& msg0, const M1& msg1) = 0;

  [[nodiscard]] virtual std::string_view out_channel() const = 0;
  [[nodiscard]] virtual std::size_t queue_depth() const { return 16; }

  void publish(const TOut& msg) { writer_->write(msg); }

 public:
  void set_out_channel_override(const std::string& channel) override {
    out_channel_override_ = channel;
  }

 private:
  void run_proc() {
    M0* msg0 = nullptr;
    M1* msg1 = nullptr;
    while ((msg0 = visitor_->try_fetch_0()) != nullptr &&
           (msg1 = visitor_->try_fetch_1()) != nullptr) {
      proc(*msg0, *msg1);
    }
  }

  void bridge_input(std::string_view channel) {
    const ChannelId id = channel_id_for(channel);
    auto reader = node_->create_reader(channel);
    reader->set_callback([id](const transport::Message& msg) {
      DataDispatcher::instance().dispatch(id, msg.data, msg.size);
    });
    input_readers_.push_back(std::move(reader));
  }

  Node* node_{nullptr};
  std::unique_ptr<Writer<TOut>> writer_;
  std::unique_ptr<DataVisitor<M0, M1>> visitor_;
  std::vector<std::unique_ptr<transport::ReaderBase>> input_readers_;
  std::string out_channel_override_;

  [[nodiscard]] std::string effective_out_channel() const {
    return out_channel_override_.empty() ? std::string(out_channel()) : out_channel_override_;
  }
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

  bool launch(Node& /*node*/, const std::vector<std::string>& /*input_channels*/,
              std::chrono::milliseconds interval) override {
    return start(interval);
  }

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

  void quiesce() override { stop(); }

 protected:
  virtual void proc() = 0;

 private:
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

// TimerSourceComponent: periodic publisher (sensor-driver pattern), the
// typical DAG entry node.
template <typename TOut>
class TimerSourceComponent : public TimerComponent {
 public:
  explicit TimerSourceComponent(std::string name) : TimerComponent(std::move(name)) {}

  bool launch(Node& node, const std::vector<std::string>& /*input_channels*/,
              std::chrono::milliseconds interval) override {
    node_ = &node;
    if (!effective_out_channel().empty()) {
      writer_ = node_->create_typed_writer<TOut>(effective_out_channel());
    }
    return TimerComponent::start(interval);
  }

 protected:
  void publish(const TOut& msg) {
    if (writer_) {
      writer_->write(msg);
    }
  }

  [[nodiscard]] virtual std::string_view out_channel() const = 0;

 public:
  void set_out_channel_override(const std::string& channel) override {
    out_channel_override_ = channel;
  }

 protected:
  Node* node_{nullptr};
  std::unique_ptr<Writer<TOut>> writer_;
  std::string out_channel_override_;

  [[nodiscard]] std::string effective_out_channel() const {
    return out_channel_override_.empty() ? std::string(out_channel()) : out_channel_override_;
  }
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
