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

// Unit tests for DagConfig parsing + Launcher (L4-MAIN-1).

#include "tianshu/core/launcher.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>

#include <gtest/gtest.h>
#include <sys/wait.h>

#include "tianshu/core/component.h"
#include "tianshu/core/message_traits.h"

// NOLINTNEXTLINE(misc-use-internal-linkage)  // traits specialization must precede template use
struct TickMsg {
  std::uint64_t seq;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint as TickMsg
struct EchoMsg {
  std::uint64_t seq;
};

// Traits before any component template use (MessageConcept instantiates
// MessageTraits in member declarations).
TIANSHU_TRAITS_POD(TickMsg, "test.TickMsg");
TIANSHU_TRAITS_POD(EchoMsg, "test.EchoMsg");

namespace {

class TestSource : public tianshu::core::TimerSourceComponent<TickMsg> {
 public:
  explicit TestSource(std::string name) : TimerSourceComponent(std::move(name)) {}
  std::atomic<std::uint64_t> emitted{0};

 protected:
  void proc() override {
    const auto seq = emitted.fetch_add(1);
    publish(TickMsg{.seq = seq});
  }

  [[nodiscard]] std::string_view out_channel() const override { return "/t/source"; }
};

class TestEcho : public tianshu::core::Component<TickMsg> {
 public:
  explicit TestEcho(std::string name) : Component(std::move(name)) {}
  std::atomic<std::uint64_t> received{0};

 protected:
  void proc(const TickMsg& msg) override {
    received.fetch_add(1);
    publish(TickMsg{.seq = msg.seq});
  }

  [[nodiscard]] std::string_view out_channel() const override { return "/t/echo"; }
};

}  // namespace

TIANSHU_REGISTER_COMPONENT(TestSource, "test_source")
TIANSHU_REGISTER_COMPONENT(TestEcho, "test_echo")

TEST(DagConfigTest, ParsesFullDag) {
  const auto dag = tianshu::core::DagConfig::parse(R"CFG(
# comment line
[component src]
type = test_source
interval_ms = 100

[component pipe]
type = test_echo
inputs = /t/source, /t/other
)CFG");
  ASSERT_TRUE(dag.ok()) << dag.error;
  ASSERT_EQ(dag.components.size(), 2U);
  EXPECT_EQ(dag.components[0].name, "src");
  EXPECT_EQ(dag.components[0].type, "test_source");
  EXPECT_EQ(dag.components[0].interval, std::chrono::milliseconds(100));
  EXPECT_TRUE(dag.components[0].input_channels.empty());
  EXPECT_EQ(dag.components[1].name, "pipe");
  ASSERT_EQ(dag.components[1].input_channels.size(), 2U);
  EXPECT_EQ(dag.components[1].input_channels[0], "/t/source");
  EXPECT_EQ(dag.components[1].input_channels[1], "/t/other");
}

TEST(DagConfigTest, RejectsUnknownKey) {
  const auto dag = tianshu::core::DagConfig::parse("[component a]\nfoo = 1\n");
  EXPECT_FALSE(dag.ok());
  EXPECT_NE(dag.error.find("unknown key"), std::string::npos);
}

TEST(DagConfigTest, RejectsMissingType) {
  const auto dag = tianshu::core::DagConfig::parse("[component a]\ninterval_ms = 5\n");
  EXPECT_FALSE(dag.ok());
  EXPECT_NE(dag.error.find("missing type"), std::string::npos);
}

TEST(DagConfigTest, RejectsKeyOutsideSection) {
  const auto dag = tianshu::core::DagConfig::parse("type = orphan\n");
  EXPECT_FALSE(dag.ok());
}

TEST(DagConfigTest, RejectsBadInterval) {
  const auto dag = tianshu::core::DagConfig::parse("[component a]\ntype = t\ninterval_ms = abc\n");
  EXPECT_FALSE(dag.ok());
}

TEST(DagConfigTest, MissingFile) {
  const auto dag = tianshu::core::DagConfig::parse_file("/nonexistent/hello.flow");
  EXPECT_FALSE(dag.ok());
}

TEST(LauncherTest, RunsSourceThroughEchoDag) {
  const auto dag = tianshu::core::DagConfig::parse(R"CFG(
[component src]
type = test_source
interval_ms = 20

[component echo]
type = test_echo
inputs = /t/source
)CFG");
  ASSERT_TRUE(dag.ok()) << dag.error;

  tianshu::core::Launcher launcher;
  std::string error;
  ASSERT_TRUE(launcher.start(dag, &error)) << error;
  ASSERT_EQ(launcher.components().size(), 2U);

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  std::uint64_t emitted = 0;
  std::uint64_t echoed = 0;
  for (const auto& comp : launcher.components()) {
    if (const auto* src = dynamic_cast<const TestSource*>(comp.get())) {
      emitted = src->emitted.load();
    } else if (const auto* echo = dynamic_cast<const TestEcho*>(comp.get())) {
      echoed = echo->received.load();
    }
  }
  launcher.stop();

  EXPECT_GE(emitted, 5);
  EXPECT_GE(echoed, emitted - 1);
}

TEST(LauncherTest, UnknownTypeFailsWithCleanError) {
  const auto dag = tianshu::core::DagConfig::parse("[component x]\ntype = nope\n");
  ASSERT_TRUE(dag.ok());
  tianshu::core::Launcher launcher;
  std::string error;
  EXPECT_FALSE(launcher.start(dag, &error));
  EXPECT_NE(error.find("unknown component type"), std::string::npos);
}

TEST(LauncherTest, ComponentMissingInputsFails) {
  const auto dag = tianshu::core::DagConfig::parse("[component x]\ntype = test_echo\ninputs =\n");
  ASSERT_TRUE(dag.ok());
  tianshu::core::Launcher launcher;
  std::string error;
  EXPECT_FALSE(launcher.start(dag, &error));
  EXPECT_NE(error.find("launch failed"), std::string::npos);
}

// ti dispatcher smoke: `ti launch ...` must exec ti-launch from PATH.
TEST(TiDispatchTest, LaunchesTiLaunchFromPath) {
  const char* ti_dir = getenv("TI_BIN_DIR");
  if (ti_dir == nullptr) {
    GTEST_SKIP() << "TI_BIN_DIR not set (run via ctest)";
  }
  const pid_t pid = fork();  // NOLINT(misc-include-cleaner)  // glibc: sys/types + unistd
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    std::string path = std::string("PATH=") + ti_dir + ":/usr/bin:/bin";
    static_cast<void>(putenv(path.data()));  // NOLINT(misc-include-cleaner)  // glibc: stdlib
    execl((std::string(ti_dir) + "/ti").c_str(), "ti", "launch", "/nonexistent.flow",
          static_cast<char*>(nullptr));
    _exit(126);
  }
  int status = 0;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  // ti-launch reports the missing file with exit 1; a 127 would mean ti
  // itself failed to dispatch.
  EXPECT_TRUE(WIFEXITED(status));     // NOLINT(misc-include-cleaner)  // glibc: bits/waitflags
  EXPECT_EQ(WEXITSTATUS(status), 1);  // NOLINT(misc-include-cleaner)  // glibc: bits/waitflags
}
