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

// Message traits: serialization and type metadata per message format.
//
// Design (per L4-CORE-1, ADR-0008):
//   - Primary template is undefined — user must specialize or use a registration macro
//   - POD specialization auto-applies to std::trivially_copyable types
//   - FlatBuffers / Protobuf specializations added in L4-CORE-11/12
//   - All specializations must satisfy MessageConcept (see message_concept.h)
//   - TIANSHU_TRAITS_POD explicit specialization overrides the auto-POD partial specialization

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace tianshu::core {

template <typename T>
struct MessageTraits;

template <typename T>
  requires std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T>
struct MessageTraits<T> {
  static constexpr bool is_zero_copy = true;

  static constexpr std::string_view name() { return "pod"; }

  static constexpr std::size_t max_serialized_size() { return sizeof(T); }

  static std::size_t serialize(const T& msg, std::uint8_t* buf, std::size_t buf_size) {
    if (buf_size < sizeof(T)) {
      return 0;
    }
    std::memcpy(buf, &msg, sizeof(T));
    return sizeof(T);
  }

  static const T* deserialize(const std::uint8_t* buf, std::size_t buf_size) {
    if (buf_size < sizeof(T)) {
      return nullptr;
    }
    return reinterpret_cast<const T*>(buf);
  }
};

}  // namespace tianshu::core

#define TIANSHU_TRAITS_POD(TypeName, TypeNameStr)                                                \
  namespace tianshu::core {                                                                      \
  template <>                                                                                    \
  struct MessageTraits<TypeName> {                                                               \
    static constexpr bool is_zero_copy = true;                                                   \
    static constexpr std::string_view name() { return TypeNameStr; }                             \
    static constexpr std::size_t max_serialized_size() { return sizeof(TypeName); }              \
    static std::size_t serialize(const TypeName& msg, std::uint8_t* buf, std::size_t buf_size) { \
      if (buf_size < sizeof(TypeName)) {                                                         \
        return 0;                                                                                \
      }                                                                                          \
      std::memcpy(buf, &msg, sizeof(TypeName));                                                  \
      return sizeof(TypeName);                                                                   \
    }                                                                                            \
    static const TypeName* deserialize(const std::uint8_t* buf, std::size_t buf_size) {          \
      if (buf_size < sizeof(TypeName)) {                                                         \
        return nullptr;                                                                          \
      }                                                                                          \
      return reinterpret_cast<const TypeName*>(buf);                                             \
    }                                                                                            \
  };                                                                                             \
  }
