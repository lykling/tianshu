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

// Typed reader: receives and deserializes T from the transport layer.
//
// Design (per L4-CORE-3, ADR-0008):
//   - Reader<T> wraps an untyped ReaderBase from the transport layer
//   - Copies raw bytes from Message into an internal buffer on callback
//   - Deserializes via MessageTraits<T>::deserialize() on try_fetch()
//   - T must satisfy MessageConcept

#pragma once

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
class Reader {
 public:
  Reader(std::unique_ptr<transport::ReaderBase> base, std::string channel)
      : base_(std::move(base)), channel_(std::move(channel)) {
    base_->set_callback([this](const transport::Message& msg) {
      if (msg.data != nullptr && msg.size > 0) {
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        buffer_.assign(static_cast<const std::uint8_t*>(msg.data),
                       static_cast<const std::uint8_t*>(msg.data) + msg.size);
        // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        last_seq_ = msg.seq;
        last_timestamp_ = msg.timestamp_ns;
        has_data_ = true;
      }
    });
  }

  const T* try_fetch() {
    if (!has_data_) {
      return nullptr;
    }
    return MessageTraits<T>::deserialize(buffer_.data(), buffer_.size());
  }

  std::string_view channel() const { return channel_; }

  uint64_t last_seq() const { return last_seq_; }
  int64_t last_timestamp() const { return last_timestamp_; }

 private:
  std::unique_ptr<transport::ReaderBase> base_;
  std::string channel_;
  std::vector<std::uint8_t> buffer_;
  uint64_t last_seq_{0};
  int64_t last_timestamp_{0};
  bool has_data_{false};
};

}  // namespace tianshu::core
