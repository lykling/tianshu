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

// Typed writer: serializes T and sends via the transport layer.
//
// Design (per L4-CORE-2, ADR-0008):
//   - Writer<T> wraps an untyped WriterBase from the transport layer
//   - Serializes T via MessageTraits<T>::serialize() before calling WriterBase::write()
//   - Maintains an internal serialization buffer (reused across calls)
//   - T must satisfy MessageConcept

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tianshu/core/message_concept.h"
#include "tianshu/core/message_traits.h"
#include "tianshu/transport/transport_backend.h"

namespace tianshu::core {

template <MessageConcept T>
class Writer {
 public:
  Writer(std::unique_ptr<transport::WriterBase> base, std::string channel)
      : base_(std::move(base)), channel_(std::move(channel)) {
    buffer_.resize(MessageTraits<T>::max_serialized_size());
  }

  // Lineage-carrying write: `lineage_ptr` must point at a core::Lineage
  // that outlives this synchronous call (component publish pairs it with
  // the input message being processed, ADR-0025 correction).
  void write(const T& msg, const void* lineage_ptr) {
    const std::size_t size = MessageTraits<T>::serialize(msg, buffer_.data(), buffer_.size());
    if (size > 0) {
      base_->write(buffer_.data(), size, lineage_ptr);
    }
  }

  void write(const T& msg) {
    const std::size_t size = MessageTraits<T>::serialize(msg, buffer_.data(), buffer_.size());
    if (size > 0) {
      base_->write(buffer_.data(), size);
    }
  }

  std::string_view channel() const { return channel_; }

 private:
  std::unique_ptr<transport::WriterBase> base_;
  std::string channel_;
  std::vector<std::uint8_t> buffer_;
};

}  // namespace tianshu::core
