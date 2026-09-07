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

// Codegen (ADR-0030 D1/D4, M-B scope): emits a self-contained
// compilation unit for one flow. The topology becomes compile-time
// constants; install replays the flow's own stage closures in
// canonical topological order — the same wire() calls the interpreter
// makes, inlined into straight-line code with constant indices.
// Per-message path specialization is the M-C / H2 milestone.

#pragma once

#include <string>

#include "tianshu/compiler/ir.h"

namespace tianshu::compiler {

// Emits the .gen.cc body for `graph` (expected normalized). Exports:
//   extern "C" const char* tianshu_flow_hash()
//   extern "C" void tianshu_flow_install(FlowRuntime*, const Flow*)
[[nodiscard]] std::string generate_source(const IrGraph& graph);

}  // namespace tianshu::compiler
