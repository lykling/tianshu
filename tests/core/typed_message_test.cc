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

// Unit tests for MessageTraits, MessageConcept, typed Reader/Writer (L4-CORE-1/2/3/10).

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "tianshu/core/message_concept.h"
#include "tianshu/core/message_traits.h"
#include "tianshu/core/node.h"
#include "tianshu/core/typed_reader.h"
#include "tianshu/core/typed_writer.h"

namespace {

struct ImuData {
  double timestamp;
  double ax;
  double ay;
  double az;
  double gx;
  double gy;
  double gz;
};

struct SimplePose {
  float x;
  float y;
  float theta;
};

}  // namespace

TIANSHU_TRAITS_POD(ImuData, "tianshu.pod.ImuData");

namespace {

static_assert(tianshu::core::MessageConcept<ImuData>);
static_assert(tianshu::core::MessageConcept<SimplePose>);
static_assert(tianshu::core::MessageConcept<int32_t>);
static_assert(tianshu::core::MessageConcept<double>);

TEST(MessageTraitsTest, PodNameDefault) {
  EXPECT_EQ(tianshu::core::MessageTraits<SimplePose>::name(), "pod");
}

TEST(MessageTraitsTest, PodNameRegistered) {
  EXPECT_EQ(tianshu::core::MessageTraits<ImuData>::name(), "tianshu.pod.ImuData");
}

TEST(MessageTraitsTest, PodIsZeroCopy) {
  EXPECT_TRUE(tianshu::core::MessageTraits<ImuData>::is_zero_copy);
  EXPECT_TRUE(tianshu::core::MessageTraits<SimplePose>::is_zero_copy);
}

TEST(MessageTraitsTest, PodMaxSerializedSize) {
  EXPECT_EQ(tianshu::core::MessageTraits<ImuData>::max_serialized_size(), sizeof(ImuData));
  EXPECT_EQ(tianshu::core::MessageTraits<SimplePose>::max_serialized_size(), sizeof(SimplePose));
}

TEST(MessageTraitsTest, PodSerializeDeserialize) {
  ImuData imu{1000.0, 1.5, -0.3, 9.8, 0.01, -0.02, 0.03};
  std::uint8_t buf[sizeof(ImuData)];
  std::size_t sz = tianshu::core::MessageTraits<ImuData>::serialize(imu, buf, sizeof(buf));
  ASSERT_EQ(sz, sizeof(ImuData));

  const auto* result = tianshu::core::MessageTraits<ImuData>::deserialize(buf, sz);
  ASSERT_NE(result, nullptr);
  EXPECT_DOUBLE_EQ(result->ax, 1.5);
  EXPECT_DOUBLE_EQ(result->az, 9.8);
}

TEST(MessageTraitsTest, PodSerializeBufferTooSmall) {
  SimplePose p{1.0f, 2.0f, 3.0f};
  std::vector<std::uint8_t> buf(sizeof(SimplePose));
  std::size_t sz = tianshu::core::MessageTraits<SimplePose>::serialize(p, buf.data(), 1);
  EXPECT_EQ(sz, 0U);
}

TEST(MessageTraitsTest, PodDeserializeBufferTooSmall) {
  std::vector<std::uint8_t> buf(sizeof(SimplePose));
  auto* result = tianshu::core::MessageTraits<SimplePose>::deserialize(buf.data(), 1);
  EXPECT_EQ(result, nullptr);
}

TEST(MessageTraitsTest, PodBuiltinTypes) {
  int32_t val = 42;
  std::uint8_t buf[sizeof(int32_t)];
  auto sz = tianshu::core::MessageTraits<int32_t>::serialize(val, buf, sizeof(buf));
  EXPECT_EQ(sz, sizeof(int32_t));

  auto* result = tianshu::core::MessageTraits<int32_t>::deserialize(buf, sz);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(*result, 42);
}

TEST(TypedWriterReaderTest, EndToEndOverIntra) {
  tianshu::core::Node node;
  auto writer = node.create_typed_writer<ImuData>("/typed/e2e");
  auto reader = node.create_typed_reader<ImuData>("/typed/e2e");

  ImuData sent{2000.0, 2.0, 3.0, 9.8, 0.1, 0.2, 0.3};
  writer->write(sent);

  const ImuData* received = reader->try_fetch();
  ASSERT_NE(received, nullptr);
  EXPECT_DOUBLE_EQ(received->timestamp, 2000.0);
  EXPECT_DOUBLE_EQ(received->ax, 2.0);
  EXPECT_DOUBLE_EQ(received->gz, 0.3);
}

TEST(TypedWriterReaderTest, MultipleMessages) {
  tianshu::core::Node node;
  auto writer = node.create_typed_writer<SimplePose>("/typed/multi");
  auto reader = node.create_typed_reader<SimplePose>("/typed/multi");

  for (int i = 0; i < 5; ++i) {
    SimplePose p{static_cast<float>(i), static_cast<float>(i * 2), static_cast<float>(i * 3)};
    writer->write(p);
    const SimplePose* result = reader->try_fetch();
    ASSERT_NE(result, nullptr);
    EXPECT_FLOAT_EQ(result->x, static_cast<float>(i));
  }
}

TEST(TypedWriterReaderTest, TryFetchBeforeWrite) {
  tianshu::core::Node node;
  auto reader = node.create_typed_reader<SimplePose>("/typed/empty");
  EXPECT_EQ(reader->try_fetch(), nullptr);
}

TEST(TypedWriterReaderTest, ChannelName) {
  tianshu::core::Node node;
  auto writer = node.create_typed_writer<ImuData>("/typed/channel");
  auto reader = node.create_typed_reader<ImuData>("/typed/channel");
  EXPECT_EQ(writer->channel(), "/typed/channel");
  EXPECT_EQ(reader->channel(), "/typed/channel");
}

TEST(TypedWriterReaderTest, SeqNumberIncrements) {
  tianshu::core::Node node;
  auto writer = node.create_typed_writer<SimplePose>("/typed/seq");
  auto reader = node.create_typed_reader<SimplePose>("/typed/seq");

  writer->write(SimplePose{1, 2, 3});
  reader->try_fetch();
  uint64_t seq1 = reader->last_seq();

  writer->write(SimplePose{4, 5, 6});
  reader->try_fetch();
  uint64_t seq2 = reader->last_seq();

  EXPECT_GT(seq2, seq1);
}

TEST(TypedWriterReaderTest, SeparateChannels) {
  tianshu::core::Node node;
  auto w_a = node.create_typed_writer<SimplePose>("/typed/a");
  auto r_a = node.create_typed_reader<SimplePose>("/typed/a");
  auto w_b = node.create_typed_writer<ImuData>("/typed/b");
  auto r_b = node.create_typed_reader<ImuData>("/typed/b");

  w_a->write(SimplePose{1, 2, 3});
  w_b->write(ImuData{100, 4, 5, 6, 7, 8, 9});

  const auto* pa = r_a->try_fetch();
  ASSERT_NE(pa, nullptr);
  EXPECT_FLOAT_EQ(pa->x, 1.0f);

  const auto* pb = r_b->try_fetch();
  ASSERT_NE(pb, nullptr);
  EXPECT_DOUBLE_EQ(pb->ax, 4.0);
}

}  // namespace
