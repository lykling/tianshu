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

#include "tianshu/core/field_table.h"

#include <cstddef>
#include <cstdint>
#include <utility>

#include <gtest/gtest.h>

namespace {

struct PodMsg {
  double d;
  float f;
  std::int32_t i32;
  std::int64_t i64;
  std::uint32_t u32;
  std::uint64_t u64;
  bool b;
  double vec[3];
};

}  // namespace

TIANSHU_TRAITS_POD_FIELDS(PodMsg, "test.PodMsg", TIANSHU_FIELD(PodMsg, d, Double),
                          TIANSHU_FIELD(PodMsg, f, Float), TIANSHU_FIELD(PodMsg, i32, Int32),
                          TIANSHU_FIELD(PodMsg, i64, Int64), TIANSHU_FIELD(PodMsg, u32, Uint32),
                          TIANSHU_FIELD(PodMsg, u64, Uint64), TIANSHU_FIELD(PodMsg, b, Bool))

// Array fields need explicit count in v0 (macro covers scalars).
static const tianshu::core::FieldDesc EXTRA_FIELDS[] = {
    {.name = "vec",
     .offset = offsetof(PodMsg, vec),
     .type = tianshu::core::FieldType::kDouble,
     .count = 3},
};

TEST(FieldTableTest, MacroGeneratesSpecialization) {
  static_assert(tianshu::core::PodFieldTable<PodMsg>::kCount == 7);
  EXPECT_STREQ(tianshu::core::PodFieldTable<PodMsg>::kTypeName, "test.PodMsg");
  EXPECT_STREQ(tianshu::core::PodFieldTable<PodMsg>::kFields[0].name, "d");
  EXPECT_EQ(tianshu::core::PodFieldTable<PodMsg>::kFields[0].offset, offsetof(PodMsg, d));
}

TEST(FieldTableTest, MacroAutoRegistersInRegistry) {
  // The anonymous-namespace self-registration ran before main().
  EXPECT_TRUE(tianshu::core::DecoderRegistry::instance().has("test.PodMsg"));
}

TEST(FieldTableTest, DecodeAllScalarTypes) {
  const PodMsg msg{.d = 3.5,
                   .f = 1.25F,
                   .i32 = -42,
                   .i64 = -9000000000LL,
                   .u32 = 7U,
                   .u64 = 18000000000ULL,
                   .b = true,
                   .vec = {0, 0, 0}};
  auto view =
      tianshu::core::decode_pod("test.PodMsg", tianshu::core::PodFieldTable<PodMsg>::kFields,
                                tianshu::core::PodFieldTable<PodMsg>::kCount,
                                reinterpret_cast<const std::uint8_t*>(&msg), sizeof(msg));
  ASSERT_EQ(view.fields.size(), 7U);
  EXPECT_EQ(view.fields[0].name, "d");
  EXPECT_EQ(view.fields[0].text, "3.5");
  EXPECT_EQ(view.fields[1].text, "1.25");
  EXPECT_EQ(view.fields[2].text, "-42");
  EXPECT_EQ(view.fields[3].text, "-9000000000");
  EXPECT_EQ(view.fields[4].text, "7");
  EXPECT_EQ(view.fields[5].text, "18000000000");
  EXPECT_EQ(view.fields[6].text, "true");
}

TEST(FieldTableTest, DecodeInlineArray) {
  PodMsg msg{};
  msg.vec[0] = 1.0;
  msg.vec[1] = 2.0;
  msg.vec[2] = 3.0;
  auto view = tianshu::core::decode_pod("test.arr", EXTRA_FIELDS, 1,
                                        reinterpret_cast<const std::uint8_t*>(&msg), sizeof(msg));
  ASSERT_EQ(view.fields.size(), 1U);
  EXPECT_EQ(view.fields[0].name, "vec");
  EXPECT_EQ(view.fields[0].text, "[1, 2, 3]");
}

TEST(FieldTableTest, SkipsFieldsBeyondPayload) {
  const PodMsg msg{};
  // Payload claims only the first field's worth of bytes.
  auto view =
      tianshu::core::decode_pod("test.short", tianshu::core::PodFieldTable<PodMsg>::kFields,
                                tianshu::core::PodFieldTable<PodMsg>::kCount,
                                reinterpret_cast<const std::uint8_t*>(&msg), sizeof(double));
  ASSERT_EQ(view.fields.size(), 1U);
  EXPECT_EQ(view.fields[0].name, "d");
}

TEST(FieldTableTest, RegistryDecodeAndMiss) {
  const PodMsg msg{
      .d = 9.0, .f = 0.0F, .i32 = 0, .i64 = 0, .u32 = 0, .u64 = 0, .b = false, .vec = {0, 0, 0}};
  tianshu::core::FieldTreeView view;
  EXPECT_TRUE(tianshu::core::DecoderRegistry::instance().decode(
      "test.PodMsg", reinterpret_cast<const std::uint8_t*>(&msg), sizeof(msg), &view));
  EXPECT_EQ(view.fields[0].text, "9");
  EXPECT_FALSE(tianshu::core::DecoderRegistry::instance().decode(
      "test.Missing", reinterpret_cast<const std::uint8_t*>(&msg), sizeof(msg), &view));
}

TEST(FieldTableTest, RegistryRegistrationIsIdempotent) {
  // Re-registering the same type name replaces instead of duplicating.
  tianshu::core::DecoderRegistry::instance().register_fields("test.PodMsg", EXTRA_FIELDS, 1);
  const PodMsg msg{
      .d = 1.0, .f = 0.0F, .i32 = 0, .i64 = 0, .u32 = 0, .u64 = 0, .b = false, .vec = {0, 0, 0}};
  tianshu::core::FieldTreeView view;
  EXPECT_TRUE(tianshu::core::DecoderRegistry::instance().decode(
      "test.PodMsg", reinterpret_cast<const std::uint8_t*>(&msg), sizeof(msg), &view));
  EXPECT_EQ(view.fields.size(), 1U);
  EXPECT_EQ(view.fields[0].name, "vec");
}

TEST(FieldTableTest, SchemaCodecRoundtrip) {
  const auto blob = tianshu::core::encode_pod_schema("test.codec.PodMsg",
                                                     tianshu::core::PodFieldTable<PodMsg>::kFields,
                                                     tianshu::core::PodFieldTable<PodMsg>::kCount);
  tianshu::core::OwnedSchemaTable table;
  ASSERT_TRUE(tianshu::core::decode_pod_schema(blob.data(), blob.size(), &table));
  EXPECT_EQ(table.type_name, "test.codec.PodMsg");
  ASSERT_EQ(table.fields.size(), tianshu::core::PodFieldTable<PodMsg>::kCount);
  for (std::size_t i = 0; i < table.fields.size(); ++i) {
    EXPECT_STREQ(table.fields[i].name, tianshu::core::PodFieldTable<PodMsg>::kFields[i].name);
    EXPECT_EQ(table.fields[i].offset, tianshu::core::PodFieldTable<PodMsg>::kFields[i].offset);
    EXPECT_EQ(table.fields[i].type, tianshu::core::PodFieldTable<PodMsg>::kFields[i].type);
    EXPECT_EQ(table.fields[i].count, tianshu::core::PodFieldTable<PodMsg>::kFields[i].count);
  }
}

TEST(FieldTableTest, OwnedSchemaDecodesViaRegistry) {
  // Type name with no compile-time table: proves the owned-storage path.
  const auto blob = tianshu::core::encode_pod_schema("test.owned.ViaBlob", EXTRA_FIELDS, 1);
  tianshu::core::OwnedSchemaTable table;
  ASSERT_TRUE(tianshu::core::decode_pod_schema(blob.data(), blob.size(), &table));
  tianshu::core::DecoderRegistry::instance().register_schema(std::move(table));
  const PodMsg msg{.d = 0.0,
                   .f = 0.0F,
                   .i32 = 0,
                   .i64 = 0,
                   .u32 = 0,
                   .u64 = 0,
                   .b = false,
                   .vec = {1.5, 2.5, 3.5}};
  tianshu::core::FieldTreeView view;
  EXPECT_TRUE(tianshu::core::DecoderRegistry::instance().decode(
      "test.owned.ViaBlob", reinterpret_cast<const std::uint8_t*>(&msg), sizeof(msg), &view));
  ASSERT_EQ(view.fields.size(), 1U);
  EXPECT_EQ(view.fields[0].name, "vec");
  EXPECT_EQ(view.fields[0].text, "[1.5, 2.5, 3.5]");
}

TEST(FieldTableTest, SchemaCodecRejectsMalformedBlobs) {
  const auto blob = tianshu::core::encode_pod_schema("test.codec.PodMsg",
                                                     tianshu::core::PodFieldTable<PodMsg>::kFields,
                                                     tianshu::core::PodFieldTable<PodMsg>::kCount);
  tianshu::core::OwnedSchemaTable table;
  EXPECT_FALSE(tianshu::core::decode_pod_schema(blob.data(), blob.size() - 3, &table));
  auto bad = blob;
  bad[0] ^= static_cast<std::uint8_t>(0xFF);
  EXPECT_FALSE(tianshu::core::decode_pod_schema(bad.data(), bad.size(), &table));
}
