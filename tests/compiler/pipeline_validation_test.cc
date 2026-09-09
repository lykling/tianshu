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

// Pipeline validation and degradation-path tests (ADR-0030 D6):
// structural rejection, strict-mode throws, move semantics, and the
// name sanitizer — the branches the happy-path pipeline tests skip.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "tianshu/compiler/ir.h"
#include "tianshu/compiler/pipeline.h"
#include "tianshu/core/lineage.h"
#include "tianshu/core/message_traits.h"
#include "tianshu/dsl/flow.h"
// Link-time requirement: the wire factory templates live here.
// NOLINTNEXTLINE(misc-include-cleaner)
#include "tianshu/dsl/dsl_runtime.h"

// NOLINTNEXTLINE(misc-use-internal-linkage)  // traits must precede template use
struct PipeTick2 {
  std::uint64_t tick;
};
// NOLINTNEXTLINE(misc-use-internal-linkage)  // same ordering constraint
struct PipeDouble2 {
  std::uint64_t tick;
};

TIANSHU_TRAITS_POD(PipeTick2, "pipe2.TickMsg");
TIANSHU_TRAITS_POD(PipeDouble2, "pipe2.DoubledMsg");

namespace {

namespace compiler = tianshu::compiler;
namespace dsl = tianshu::dsl;

// Fault injection via friendship: the public IR is immutable. The peer
// must live in the IR's namespace for the friend declaration to bind.
}  // namespace

namespace tianshu::compiler {
class IrGraphTestPeer {
 public:
  static IrGraph with_nodes(std::vector<IrNode> nodes) {
    IrGraph graph;
    graph.nodes_ = std::move(nodes);
    return graph;
  }
};
}  // namespace tianshu::compiler

namespace {

using ::tianshu::compiler::IrGraphTestPeer;

compiler::IrNode make_node(const std::string& kind, std::vector<std::string> inputs,
                           const std::string& output) {
  compiler::IrNode node;
  node.kind = kind;
  node.inputs = std::move(inputs);
  node.output = output;
  node.type_name = "t";
  return node;
}

std::string writable_dir() {
  const char* tmp = std::getenv("TEST_TMPDIR");
  return tmp != nullptr ? std::string(tmp) : std::string("/tmp");
}

compiler::CompileOptions dir_opts(const std::string& tag) {
  compiler::CompileOptions opts;
  opts.cache_dir = writable_dir() + "/tianshu-pv-" + tag;
  return opts;
}

compiler::CompileOptions strict_opts(const std::string& cc) {
  compiler::CompileOptions opts;
  opts.cache_dir = writable_dir() + "/tianshu-pv-strict";
  opts.compiler = cc;
  opts.allow_fallback = false;
  return opts;
}

TEST(PipelineValidationTest, DanglingInputRejected) {
  // A graph whose only map input is produced by nothing: fabricated by
  // lowering a hand-built IR (the DSL cannot express dangling wiring).
  const auto graph = IrGraphTestPeer::with_nodes({make_node("map", {"ghost/in"}, "out")});
  EXPECT_THROW(static_cast<void>(compiler::Pipeline::validate(graph)), std::invalid_argument);
}

TEST(PipelineValidationTest, DuplicateProducerRejected) {
  const auto graph =
      IrGraphTestPeer::with_nodes({make_node("source", {}, "a"), make_node("map", {"a"}, "dup"),
                                   make_node("map", {"a"}, "dup")});
  EXPECT_THROW(static_cast<void>(compiler::Pipeline::validate(graph)), std::invalid_argument);
}

TEST(PipelineValidationTest, WellFormedGraphPasses) {
  const auto graph = IrGraphTestPeer::with_nodes(
      {make_node("source", {}, "a"), make_node("map", {"a"}, "b"), make_node("sink", {"b"}, "")});
  EXPECT_NO_THROW(static_cast<void>(compiler::Pipeline::validate(graph)));
}

TEST(PipelineStrictTest, MissingCompilerThrowsWhenFallbackDisallowed) {
  std::vector<std::string> sink;
  dsl::FlowBuilder b("pipe_strict");
  b.source<PipeTick2>("t", std::chrono::milliseconds(5),
                      [](std::uint64_t t) { return PipeTick2{.tick = t}; })
      .map<PipeDouble2>([](const PipeTick2& in) { return PipeDouble2{.tick = in.tick}; })
      .sink([&sink](const PipeDouble2& msg, const tianshu::core::Lineage&) {
        sink.push_back(std::to_string(msg.tick));
      });

  EXPECT_THROW(
      static_cast<void>(compiler::Pipeline::compile(b.build(), strict_opts("/nonexistent/cc"))),
      std::runtime_error);
}

TEST(PipelineMoveTest, MovedFromIsValidAndReleasesArtifact) {
  std::vector<std::string> sink;
  dsl::FlowBuilder b("pipe_move");
  b.source<PipeTick2>("t", std::chrono::milliseconds(5),
                      [](std::uint64_t t) { return PipeTick2{.tick = t}; })
      .map<PipeDouble2>([](const PipeTick2& in) { return PipeDouble2{.tick = in.tick}; })
      .sink([&sink](const PipeDouble2& msg, const tianshu::core::Lineage&) {
        sink.push_back(std::to_string(msg.tick));
      });

  auto first = compiler::Pipeline::compile(b.build(), dir_opts("/tmp/tianshu-pipeline-move"));
  ASSERT_TRUE(first.valid());
  const std::string hash = first.hash();

  compiler::CompiledFlow second = std::move(first);
  EXPECT_TRUE(second.valid());
  // NOLINTNEXTLINE(bugprone-use-after-move)  // moved-from state is the assertion
  EXPECT_FALSE(first.valid());
  EXPECT_EQ(second.hash(), hash);

  // Move-assign over a loaded artifact: the old handle must close.
  auto third = compiler::Pipeline::compile(b.build(), dir_opts("/tmp/tianshu-pipeline-move"));
  second = std::move(third);
  EXPECT_TRUE(second.valid());

  // A moved-from default instance is reusable as a degraded shell.
  compiler::CompiledFlow shell;
  const compiler::CompiledFlow shell2 = std::move(shell);
  EXPECT_FALSE(shell2.valid());
}

}  // namespace
