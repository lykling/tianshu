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

// Hello DAG: the first TIANSHU component graph.
//
//   hello_source (10 Hz) --/demo/count--> hello_doubler --> /demo/doubled
//
// Embeds the components and drives the Launcher from examples/hello.flow —
// the same file format ti-launch consumes. Runs ~1.5 s and exits 0 so it
// stays CI-friendly.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "tianshu/core/component.h"
#include "tianshu/core/launcher.h"
#include "tianshu/core/message_traits.h"

// NOLINTNEXTLINE(misc-use-internal-linkage)  // traits specialization must precede template use
struct CountMsg {
  std::uint64_t seq;
  double value;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint as CountMsg
struct DoubledMsg {
  std::uint64_t seq;
  double doubled;
};

// Traits must precede any template use: the MessageConcept constraint on
// Writer<T> instantiates MessageTraits<T> as soon as the component classes
// below name Writer<CountMsg> in their member declarations.
TIANSHU_TRAITS_POD(CountMsg, "demo.CountMsg");
TIANSHU_TRAITS_POD(DoubledMsg, "demo.DoubledMsg");

namespace {

class HelloSource : public tianshu::core::TimerSourceComponent<CountMsg> {
 public:
  explicit HelloSource(std::string name) : TimerSourceComponent(std::move(name)) {}
  std::atomic<std::uint64_t> emitted{0};

 protected:
  void proc() override {
    const auto seq = emitted.fetch_add(1);
    publish(CountMsg{.seq = seq, .value = static_cast<double>(seq)});
  }

  [[nodiscard]] std::string_view out_channel() const override { return "/demo/count"; }
};

class HelloDoubler : public tianshu::core::Component<CountMsg, DoubledMsg> {
 public:
  explicit HelloDoubler(std::string name) : Component(std::move(name)) {}
  std::atomic<std::uint64_t> processed{0};
  std::atomic<double> last_doubled{0};

 protected:
  void proc(const CountMsg& msg) override {
    processed.fetch_add(1);
    last_doubled.store(msg.value * 2);
    publish(DoubledMsg{.seq = msg.seq, .doubled = msg.value * 2});
  }

  [[nodiscard]] std::string_view out_channel() const override { return "/demo/doubled"; }
};

}  // namespace

TIANSHU_REGISTER_COMPONENT(HelloSource, "hello_source")
TIANSHU_REGISTER_COMPONENT(HelloDoubler, "hello_doubler")

int main(int argc, char** argv) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const std::string dag_path = argc > 1 ? argv[1] : "examples/hello.flow";
  const auto dag = tianshu::core::DagConfig::parse_file(dag_path);
  if (!dag.ok()) {
    static_cast<void>(
        std::fprintf(stderr, "hello_dag: %s: %s\n", dag_path.c_str(), dag.error.c_str()));
    return 1;
  }

  tianshu::core::Launcher launcher;
  std::string error;
  if (!launcher.start(dag, &error)) {
    static_cast<void>(std::fprintf(stderr, "hello_dag: %s\n", error.c_str()));
    return 1;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(1500));

  for (const auto& comp : launcher.components()) {
    if (const auto* src = dynamic_cast<const HelloSource*>(comp.get())) {
      static_cast<void>(std::printf("hello_source: emitted=%llu\n",
                                    static_cast<unsigned long long>(src->emitted.load())));
    } else if (const auto* dbl = dynamic_cast<const HelloDoubler*>(comp.get())) {
      static_cast<void>(std::printf("hello_doubler: processed=%llu last_doubled=%.1f\n",
                                    static_cast<unsigned long long>(dbl->processed.load()),
                                    dbl->last_doubled.load()));
    }
  }
  launcher.stop();
  static_cast<void>(std::printf("hello_dag done\n"));
  return 0;
}
