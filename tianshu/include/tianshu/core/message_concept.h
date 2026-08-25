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

// MessageConcept: C++20 concept constraining types usable as TIANSHU messages.
//
// Design (per L4-CORE-10, ADR-0008):
//   - A type T satisfies MessageConcept if MessageTraits<T> provides:
//     name(), kIsZeroCopy, max_serialized_size(), serialize(), deserialize()
//   - POD types auto-satisfy via the partial specialization in message_traits.h
//   - FlatBuffers / Protobuf types satisfy via explicit specialization macros

#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "tianshu/core/message_traits.h"

namespace tianshu::core {

template <typename T>
concept MessageConcept =
    requires(const T& msg, std::uint8_t* buf, std::size_t buf_size, const std::uint8_t* cbuf) {
      { MessageTraits<T>::name() } -> std::convertible_to<std::string_view>;
      { MessageTraits<T>::kIsZeroCopy } -> std::convertible_to<bool>;
      { MessageTraits<T>::max_serialized_size() } -> std::convertible_to<std::size_t>;
      { MessageTraits<T>::serialize(msg, buf, buf_size) } -> std::same_as<std::size_t>;
      { MessageTraits<T>::deserialize(cbuf, buf_size) } -> std::same_as<const T*>;
    };

}  // namespace tianshu::core
