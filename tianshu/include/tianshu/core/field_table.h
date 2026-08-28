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

// POD field table: compile-time field descriptors for runtime decoding
// (per ADR-0020 Phase 1).
//
// Design:
//   - FieldDesc: (name, offset, type, count) — enough to walk POD bytes
//   - PodFieldTable<T>: opt-in specialization via TIANSHU_TRAITS_POD_FIELDS
//   - decode_pod(): payload bytes -> FieldTreeView (format-neutral)
//   - DecoderRegistry: (type_name, FieldDesc*) lookup for tools that
//     know the type name but not the C++ type
//
// Cross-process: a compiled-in table can also be encoded into a schema
// blob (encode_pod_schema) and shipped beside the channel; receivers
// decode it (decode_pod_schema) and register the owned table — the tool
// binary no longer needs to link the publisher's types.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace tianshu::core {

enum class FieldType : std::uint8_t {
  kDouble,
  kFloat,
  kInt32,
  kInt64,
  kUint32,
  kUint64,
  kBool,
};

struct FieldDesc {
  const char* name;
  std::size_t offset;
  FieldType type;
  std::size_t count;  // 1 = scalar; >1 = inline array
};

// Format-neutral decoded view (ADR-0020: tools consume only this).
struct FieldValue {
  std::string name;
  std::string text;  // formatted value ("3.140", "42", "true")
};

struct FieldTreeView {
  std::string type_name;
  std::vector<FieldValue> fields;
};

// Primary template: no fields (detected via SFINAE / requires).
template <typename T>
struct PodFieldTable;  // intentionally undefined

// --- decode ---------------------------------------------------------------

// Walks `payload` using the field descriptors. Fields whose range
// exceeds `payload_size` are skipped (defensive against schema drift).
FieldTreeView decode_pod(std::string_view type_name, const FieldDesc* fields,
                         std::size_t field_count, const std::uint8_t* payload,
                         std::size_t payload_size);

// True when T opted in via TIANSHU_TRAITS_POD_FIELDS (SFINAE-detected).
template <typename T, typename = void>
struct HasPodFieldTable : std::false_type {};

template <typename T>
struct HasPodFieldTable<T, std::void_t<decltype(PodFieldTable<T>::kFields)>> : std::true_type {};

// --- schema blob codec (ADR-0020 Phase 2) ----------------------------------
//
// Wire format (little-endian, fixed):
//   [u64 magic][u16 type_name_len][type_name][u32 field_count]
//   per field: [u16 name_len][name][u64 offset][u8 type][u64 count]

inline constexpr std::uint64_t kPodSchemaMagic = 0x54534843'4d303201ULL;

// Runtime-loaded table: `names` owns the storage; `fields[i].name`
// points into `names[i]` after decode_pod_schema (deque keeps element
// addresses stable across move and growth).
struct OwnedSchemaTable {
  std::string type_name;
  std::deque<std::string> names;
  std::vector<FieldDesc> fields;
};

[[nodiscard]] std::vector<std::uint8_t> encode_pod_schema(std::string_view type_name,
                                                          const FieldDesc* fields,
                                                          std::size_t field_count);

// Defensive parse: returns false on any truncation or bad magic.
[[nodiscard]] bool decode_pod_schema(const std::uint8_t* blob, std::size_t size,
                                     OwnedSchemaTable* out);

// --- registry -------------------------------------------------------------

// Runtime registry for tools that look up by type name (ti-monitor).
class DecoderRegistry {
 public:
  static DecoderRegistry& instance() noexcept;

  // noexcept: called from static initializers (TIANSHU_TRAITS_POD_FIELDS);
  // a failed registration is non-fatal — decode() falls back to hex dump.
  void register_fields(std::string_view type_name, const FieldDesc* fields,
                       std::size_t count) noexcept;

  // Runtime-loaded schema (sidecar segment, ADR-0020 Phase 2). Same
  // best-effort semantics as register_fields; replaces any prior entry
  // for the same type name.
  void register_schema(OwnedSchemaTable&& table) noexcept;

  // Returns true and fills `out` on success; false when the type has no
  // registered table (caller falls back to hex dump).
  bool decode(std::string_view type_name, const std::uint8_t* payload, std::size_t payload_size,
              FieldTreeView* out) const;

  [[nodiscard]] bool has(std::string_view type_name) const;

 private:
  DecoderRegistry() = default;

  struct Entry {
    std::string type_name;
    const FieldDesc* fields{nullptr};
    std::size_t count{0};
    OwnedSchemaTable owned;
  };

  std::vector<Entry> entries_;
};

// --- convenience macros ----------------------------------------------------

#define TIANSHU_FIELD(Type, fieldName, fieldTypeEnum) \
  {#fieldName, offsetof(Type, fieldName), ::tianshu::core::FieldType::k##fieldTypeEnum, 1}

// Opt a POD type into field decoding. Must appear at global scope,
// after the type definition, before any use.
//
//   struct ImuData { double timestamp; double ax; double az; };
//   TIANSHU_TRAITS_POD_FIELDS(ImuData, "demo.ImuData",
//       TIANSHU_FIELD(ImuData, timestamp, Double),
//       TIANSHU_FIELD(ImuData, ax, Double),
//       TIANSHU_FIELD(ImuData, az, Double))
#define TIANSHU_TRAITS_POD_FIELDS(Type, TypeNameStr, ...)                                        \
  template <>                                                                                    \
  struct tianshu::core::PodFieldTable<Type> {                                                    \
    [[maybe_unused]] static constexpr const char* kTypeName = TypeNameStr;                       \
    [[maybe_unused]] static constexpr ::tianshu::core::FieldDesc kFields[] = {__VA_ARGS__};      \
    [[maybe_unused]] static constexpr std::size_t kCount = sizeof(kFields) / sizeof(kFields[0]); \
  };                                                                                             \
  namespace {                                                                                    \
  [[maybe_unused]] const bool Type##_fields_registered = []() noexcept {                         \
    ::tianshu::core::DecoderRegistry::instance().register_fields(                                \
        TypeNameStr, ::tianshu::core::PodFieldTable<Type>::kFields,                              \
        ::tianshu::core::PodFieldTable<Type>::kCount);                                           \
    return true;                                                                                 \
  }();                                                                                           \
  }  // namespace

}  // namespace tianshu::core
