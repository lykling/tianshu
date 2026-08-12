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

// Node: the central factory for creating readers, writers, and managing
// transport within a single component or flow function.
//
// Design (per L4-CORE-4, ADR-0010):
//   - Node owns a HybridTransport instance
//   - create_reader/create_writer delegate to transport
//   - Future: will integrate with Scheduler, DataVisitor, etc.

#pragma once

#include <memory>
#include <string_view>

#include "tianshu/transport/transport_backend.h"

namespace tianshu::transport {
class HybridTransport;
}

namespace tianshu::core {

class Node {
 public:
  Node();

  // Create a reader for a channel.
  std::unique_ptr<transport::ReaderBase> create_reader(std::string_view channel,
                                                       std::string_view msg_type = "");

  // Create a writer for a channel.
  std::unique_ptr<transport::WriterBase> create_writer(std::string_view channel,
                                                       std::string_view msg_type = "");

 private:
  std::unique_ptr<transport::TransportBackend> transport_;
};

}  // namespace tianshu::core
