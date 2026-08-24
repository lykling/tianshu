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
// Design (per L4-CORE-4, ADR-0008, ADR-0010):
//   - Node owns a HybridTransport instance
//   - TransportMode selects INTRA (same process) or SHM (cross process)
//   - Typed create_typed_reader<T>/create_typed_writer<T> wrap with serialization

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "tianshu/core/message_concept.h"
#include "tianshu/core/typed_reader.h"
#include "tianshu/core/typed_writer.h"
#include "tianshu/transport/transport_backend.h"

namespace tianshu::transport {
class HybridTransport;
}  // namespace tianshu::transport

namespace tianshu::core {

class Node {
 public:
  explicit Node(transport::TransportMode mode = transport::TransportMode::kIntra);

  std::unique_ptr<transport::ReaderBase> create_reader(std::string_view channel,
                                                       std::string_view msg_type = "");

  std::unique_ptr<transport::WriterBase> create_writer(std::string_view channel,
                                                       std::string_view msg_type = "");

  template <MessageConcept T>
  std::unique_ptr<Reader<T>> create_typed_reader(std::string_view channel) {
    return std::make_unique<Reader<T>>(create_reader(channel, MessageTraits<T>::name()),
                                       std::string(channel));
  }

  template <MessageConcept T>
  std::unique_ptr<Writer<T>> create_typed_writer(std::string_view channel) {
    return std::make_unique<Writer<T>>(create_writer(channel, MessageTraits<T>::name()),
                                       std::string(channel));
  }

 private:
  std::unique_ptr<transport::TransportBackend> transport_;
};

}  // namespace tianshu::core
