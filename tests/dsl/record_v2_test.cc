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

// Record format v2 tests: CRC, compression round-trip, channel dictionary,
// lineage serialization, chunked streaming write/read, statistics, split/merge.

#include "tianshu/dsl/record_v2.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>
// NOLINTBEGIN(bugprone-unchecked-optional-access)  // ASSERT_TRUE precedes every dereference

#include "tianshu/core/lineage.h"

namespace {

using tianshu::dsl::record::Compression;
using tianshu::dsl::record::crc32;
using tianshu::dsl::record::deserialize_lineage;
using tianshu::dsl::record::RecordedMessageV2;
using tianshu::dsl::record::RecordReader;
using tianshu::dsl::record::RecordWriter;
using tianshu::dsl::record::serialize_lineage;

constexpr const char* kTestPath = "/tmp/tianshu_record_v2_test.trec";

TEST(Crc32Test, KnownVectors) {
  EXPECT_EQ(crc32("", 0), 0U);
  // "123456789" CRC32 = 0xCBF43926 (standard check value)
  const char* check = "123456789";
  EXPECT_EQ(crc32(check, 9), 0xCBF43926U);
}

TEST(Crc32Test, DifferentDataDifferentCrc) {
  const char a[] = "hello";
  const char b[] = "world";
  EXPECT_NE(crc32(a, 5), crc32(b, 5));
}

TEST(LineageSerdeTest, RoundTrip) {
  auto lin = tianshu::core::Lineage::rooted("imu", 42);
  lin.add_hop({.channel = "fusion", .seq = 10});

  std::unordered_map<std::string, std::uint16_t> ids = {{"imu", 0}, {"fusion", 1}};
  const auto blob = serialize_lineage(lin, [&ids](const std::string& ch) { return ids.at(ch); });
  ASSERT_FALSE(blob.empty());

  std::vector<std::string> names = {"imu", "fusion"};
  const auto decoded = deserialize_lineage(
      blob, [&names](std::uint16_t id) -> const std::string& { return names[id]; });
  EXPECT_EQ(decoded.describe(), "imu#42 -> fusion#10");
}

TEST(LineageSerdeTest, RangeRoundTrip) {
  auto lin = tianshu::core::Lineage::rooted_range("imu", 100, 120);
  lin.add_hop({.channel = "comp", .seq = 5, .seq_end = 5});

  std::unordered_map<std::string, std::uint16_t> ids = {{"imu", 0}, {"comp", 1}};
  const auto blob = serialize_lineage(lin, [&ids](const std::string& ch) { return ids.at(ch); });

  std::vector<std::string> names = {"imu", "comp"};
  const auto decoded = deserialize_lineage(
      blob, [&names](std::uint16_t id) -> const std::string& { return names[id]; });
  EXPECT_EQ(decoded.describe(), "imu#100..#120 -> comp#5");
}

TEST(RecordV2Test, WriteReadRoundTrip) {
  // Write.
  {
    RecordWriter writer(kTestPath, Compression::kNone);
    const auto imu_id = writer.add_channel("test/imu", 0, "ImuMsg");
    const auto lidar_id = writer.add_channel("test/lidar", 0, "LidarMsg");

    for (int i = 0; i < 100; ++i) {
      const auto payload = static_cast<std::uint64_t>(i);
      const auto ts = static_cast<std::uint64_t>(i) * 1000000;  // 1ms apart
      auto lin = tianshu::core::Lineage::rooted("test/imu", static_cast<std::uint64_t>(i));
      writer.append(imu_id, static_cast<std::uint64_t>(i), ts, &payload, sizeof(payload), &lin);
      if (i % 10 == 0) {
        const auto lidar_payload = i * 100;
        writer.append(lidar_id, static_cast<std::uint64_t>(i / 10), ts + 500, &lidar_payload,
                      sizeof(lidar_payload));
      }
    }
    ASSERT_TRUE(writer.finish());
    EXPECT_EQ(writer.message_count(), 110U);
  }

  // Read.
  auto reader_opt = RecordReader::open(kTestPath);
  ASSERT_TRUE(reader_opt.has_value());
  auto& reader = reader_opt.value();

  // Dictionary.
  ASSERT_EQ(reader.channels().size(), 2U);
  const auto* imu = reader.find_channel("test/imu");
  ASSERT_NE(imu, nullptr);
  EXPECT_EQ(imu->name, "test/imu");
  EXPECT_EQ(imu->type_name, "ImuMsg");

  // Statistics.
  const auto& stats = reader.stats();
  EXPECT_EQ(stats.total_messages, 110U);
  EXPECT_EQ(imu->message_count, 100U);
  EXPECT_EQ(imu->seq_min, 0U);
  EXPECT_EQ(imu->seq_max, 99U);

  // Sequential read.
  std::uint64_t last_ts = 0;
  std::uint64_t imu_count = 0;
  std::uint64_t lidar_count = 0;
  RecordedMessageV2 msg;
  while (reader.next(&msg)) {
    EXPECT_GE(msg.ts_ns, last_ts);  // strict temporal ordering
    last_ts = msg.ts_ns;
    if (msg.channel_id == imu->id) {
      imu_count++;
      ASSERT_EQ(msg.payload.size(), sizeof(std::uint64_t));
      std::uint64_t val = 0;
      std::memcpy(&val, msg.payload.data(), sizeof(val));
      // Payload order may differ from seq due to chunk sorting by ts.
      EXPECT_LT(val, 100U);
      // Lineage present.
      ASSERT_TRUE(msg.lineage.has_value());
      EXPECT_NE(msg.lineage.value().describe().find("test/imu#"), std::string::npos);
    } else {
      lidar_count++;
    }
  }
  EXPECT_EQ(imu_count, 100U);
  EXPECT_EQ(lidar_count, 10U);
}

TEST(RecordV2Test, CompressedRoundTrip) {
  const char* path = "/tmp/tianshu_record_v2_lz4.trec";
  {
    RecordWriter writer(path, Compression::kLz4);
    const auto id = writer.add_channel("comp/data", 0, "BigMsg");
    // Highly compressible payload.
    std::vector<std::uint8_t> payload(1000, 0xAB);
    for (int i = 0; i < 50; ++i) {
      const auto ts = static_cast<std::uint64_t>(i) * 1000;
      writer.append(id, static_cast<std::uint64_t>(i), ts, payload.data(), payload.size());
    }
    ASSERT_TRUE(writer.finish());
  }

  auto reader_opt = RecordReader::open(path);
  ASSERT_TRUE(reader_opt.has_value());
  RecordedMessageV2 msg;
  std::uint64_t count = 0;
  while (reader_opt.value().next(&msg)) {
    EXPECT_EQ(msg.payload.size(), 1000U);
    EXPECT_EQ(msg.payload[0], 0xAB);
    count++;
  }
  EXPECT_EQ(count, 50U);
}

TEST(RecordV2Test, SplitByTime) {
  const char* input = "/tmp/tianshu_record_v2_split_in.trec";
  const char* output = "/tmp/tianshu_record_v2_split_out.trec";

  {
    RecordWriter writer(input, Compression::kNone);
    const auto id = writer.add_channel("split/ch", 0, "Msg");
    for (int i = 0; i < 100; ++i) {
      const auto payload = static_cast<std::uint64_t>(i);
      const auto ts = static_cast<std::uint64_t>(i) * 1000;
      writer.append(id, static_cast<std::uint64_t>(i), ts, &payload, sizeof(payload));
    }
    ASSERT_TRUE(writer.finish());
  }

  // Split [20000, 50000].
  ASSERT_TRUE(RecordReader::split_by_time(input, output, 20000, 50000));

  auto reader_opt = RecordReader::open(output);
  ASSERT_TRUE(reader_opt.has_value());
  const auto& stats = reader_opt.value().stats();
  // Timestamps 20000..50000 → seq 20..50 → 31 messages.
  EXPECT_EQ(stats.total_messages, 31U);
}

TEST(RecordV2Test, Merge) {
  const char* file_a = "/tmp/tianshu_record_v2_merge_a.trec";
  const char* file_b = "/tmp/tianshu_record_v2_merge_b.trec";
  const char* output = "/tmp/tianshu_record_v2_merge_out.trec";

  {
    RecordWriter a(file_a, Compression::kNone);
    const auto id = a.add_channel("merge/ch", 0, "Msg");
    for (int i = 0; i < 10; ++i) {
      const auto payload = static_cast<std::uint64_t>(i);
      const auto ts = static_cast<std::uint64_t>(i) * 1000;
      a.append(id, static_cast<std::uint64_t>(i), ts, &payload, sizeof(payload));
    }
    ASSERT_TRUE(a.finish());
  }
  {
    RecordWriter b(file_b, Compression::kNone);
    const auto id = b.add_channel("merge/ch", 0, "Msg");  // same channel
    const auto other = b.add_channel("merge/other", 0, "Other");
    for (int i = 0; i < 5; ++i) {
      const auto payload = static_cast<std::uint64_t>(i) * 100;
      const auto ts = (static_cast<std::uint64_t>(i) * 1000) + 500;
      b.append(id, static_cast<std::uint64_t>(i), ts, &payload, sizeof(payload));
    }
    const std::uint64_t other_payload = 42;
    b.append(other, 0, 100, &other_payload, sizeof(other_payload));
    ASSERT_TRUE(b.finish());
  }
  static_cast<void>(std::remove(output));
  ASSERT_TRUE(RecordReader::merge({file_a, file_b}, output));

  auto reader_opt = RecordReader::open(output);
  ASSERT_TRUE(reader_opt.has_value());
  const auto& stats = reader_opt.value().stats();
  EXPECT_EQ(stats.total_messages, 16U);  // 10 + 5 + 1
  // Merged dictionary has both channels.
  EXPECT_EQ(reader_opt.value().channels().size(), 2U);
}

TEST(RecordV2Test, EmptyFile) {
  const char* path = "/tmp/tianshu_record_v2_empty.trec";
  {
    RecordWriter writer(path, Compression::kNone);
    ASSERT_TRUE(writer.finish());
  }
  auto reader_opt = RecordReader::open(path);
  ASSERT_TRUE(reader_opt.has_value());
  EXPECT_EQ(reader_opt.value().stats().total_messages, 0U);
  RecordedMessageV2 msg;
  EXPECT_FALSE(reader_opt.value().next(&msg));
}

}  // namespace
// NOLINTEND(bugprone-unchecked-optional-access)
