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

// Unit tests for INTRA transport backend (L4-TRANS-20).

#include "tianshu/transport/intra_backend.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "tianshu/transport/transport_backend.h"

namespace {

using tianshu::transport::BackendType;
using tianshu::transport::ChannelConfig;
using tianshu::transport::Message;
using tianshu::transport::MessageCallback;

TEST(IntraBackendTest, TypeIsIntra) {
  const tianshu::transport::intra::IntraBackend backend;
  EXPECT_EQ(backend.type(), BackendType::kIntra);
  EXPECT_TRUE(backend.supports_zero_copy());
  EXPECT_FALSE(backend.supports_remote());
}

TEST(IntraBackendTest, SingleWriterSingleReader) {
  tianshu::transport::intra::IntraBackend backend;
  ChannelConfig cfg;
  cfg.channel_name = "/test/basic";

  auto writer = backend.create_writer(cfg);
  auto reader = backend.create_reader(cfg);

  std::atomic<int> count{0};
  std::string received_data;

  reader->set_callback([&](const Message& msg) {
    count.fetch_add(1);
    received_data.assign(static_cast<const char*>(msg.data), msg.size);
  });

  std::string test_msg = "hello_intra";
  writer->write(test_msg.data(), test_msg.size());

  EXPECT_EQ(count.load(), 1);
  EXPECT_EQ(received_data, test_msg);
}

TEST(IntraBackendTest, MultipleMessages) {
  tianshu::transport::intra::IntraBackend backend;
  ChannelConfig cfg;
  cfg.channel_name = "/test/multi";

  auto writer = backend.create_writer(cfg);
  auto reader = backend.create_reader(cfg);

  std::vector<std::string> received;
  reader->set_callback([&](const Message& msg) {
    received.emplace_back(static_cast<const char*>(msg.data), msg.size);
  });

  for (int i = 0; i < 10; ++i) {
    std::string s = "msg_" + std::to_string(i);
    writer->write(s.data(), s.size());
  }

  ASSERT_EQ(received.size(), 10U);
  for (std::size_t i = 0; i < 10; ++i) {
    EXPECT_EQ(received[i], "msg_" + std::to_string(i));
  }
}

TEST(IntraBackendTest, SeqNumberIncrements) {
  tianshu::transport::intra::IntraBackend backend;
  ChannelConfig cfg;
  cfg.channel_name = "/test/seq";

  auto writer = backend.create_writer(cfg);
  auto reader = backend.create_reader(cfg);

  std::vector<uint64_t> seqs;
  reader->set_callback([&](const Message& msg) { seqs.push_back(msg.seq); });

  for (int i = 0; i < 5; ++i) {
    writer->write("x", 1);
  }

  ASSERT_EQ(seqs.size(), 5U);
  for (uint64_t i = 0; i < 5; ++i) {
    EXPECT_EQ(seqs[i], i);
  }
}

TEST(IntraBackendTest, OneWriterMultipleReaders) {
  tianshu::transport::intra::IntraBackend backend;
  ChannelConfig cfg;
  cfg.channel_name = "/test/fanout";

  auto writer = backend.create_writer(cfg);
  auto reader1 = backend.create_reader(cfg);
  auto reader2 = backend.create_reader(cfg);

  std::atomic<int> count1{0};
  std::atomic<int> count2{0};

  reader1->set_callback([&](const Message&) { count1.fetch_add(1); });
  reader2->set_callback([&](const Message&) { count2.fetch_add(1); });

  writer->write("broadcast", 10);

  EXPECT_EQ(count1.load(), 1);
  EXPECT_EQ(count2.load(), 1);
}

TEST(IntraBackendTest, DifferentChannelsIndependent) {
  tianshu::transport::intra::IntraBackend backend;

  ChannelConfig cfg_a;
  cfg_a.channel_name = "/test/ch_a";
  ChannelConfig cfg_b;
  cfg_b.channel_name = "/test/ch_b";

  auto writer_a = backend.create_writer(cfg_a);
  auto reader_a = backend.create_reader(cfg_a);
  auto writer_b = backend.create_writer(cfg_b);
  auto reader_b = backend.create_reader(cfg_b);

  std::atomic<int> count_a{0};
  std::atomic<int> count_b{0};

  reader_a->set_callback([&](const Message&) { count_a.fetch_add(1); });
  reader_b->set_callback([&](const Message&) { count_b.fetch_add(1); });

  writer_a->write("a", 1);
  EXPECT_EQ(count_a.load(), 1);
  EXPECT_EQ(count_b.load(), 0);

  writer_b->write("b", 1);
  EXPECT_EQ(count_a.load(), 1);
  EXPECT_EQ(count_b.load(), 1);
}

TEST(IntraBackendTest, ReaderBeforeWriter) {
  tianshu::transport::intra::IntraBackend backend;
  ChannelConfig cfg;
  cfg.channel_name = "/test/rbw";

  auto reader = backend.create_reader(cfg);
  auto writer = backend.create_writer(cfg);

  std::atomic<int> count{0};
  reader->set_callback([&](const Message&) { count.fetch_add(1); });

  writer->write("late_writer", 11);
  EXPECT_EQ(count.load(), 1);
}

TEST(IntraBackendTest, WriteBeforeCallbackSet) {
  tianshu::transport::intra::IntraBackend backend;
  ChannelConfig cfg;
  cfg.channel_name = "/test/nocb";

  auto writer = backend.create_writer(cfg);
  auto reader = backend.create_reader(cfg);

  // Write before set_callback — should not crash (callback_ is nullopt/functionally empty).
  writer->write("before_cb", 10);

  std::atomic<int> count{0};
  reader->set_callback([&](const Message&) { count.fetch_add(1); });

  writer->write("after_cb", 9);
  EXPECT_EQ(count.load(), 1);
}

TEST(IntraBackendTest, MultipleWritersSameChannel) {
  tianshu::transport::intra::IntraBackend backend;
  ChannelConfig cfg;
  cfg.channel_name = "/test/multi_writer";

  auto writer1 = backend.create_writer(cfg);
  auto writer2 = backend.create_writer(cfg);
  auto reader = backend.create_reader(cfg);

  std::atomic<int> count{0};
  reader->set_callback([&](const Message&) { count.fetch_add(1); });

  writer1->write("from_w1", 8);
  writer2->write("from_w2", 8);

  EXPECT_EQ(count.load(), 2);
}

TEST(IntraBackendTest, MultipleWritersSeqContinuous) {
  tianshu::transport::intra::IntraBackend backend;
  ChannelConfig cfg;
  cfg.channel_name = "/test/multi_writer_seq";

  auto writer1 = backend.create_writer(cfg);
  auto writer2 = backend.create_writer(cfg);
  auto reader = backend.create_reader(cfg);

  std::vector<uint64_t> seqs;
  reader->set_callback([&](const Message& msg) { seqs.push_back(msg.seq); });

  writer1->write("a", 1);
  writer2->write("b", 1);
  writer1->write("c", 1);

  ASSERT_EQ(seqs.size(), 3U);
  // Both writer refs point to the same underlying IntraWriter via registry,
  // so seq numbers should be continuous.
  EXPECT_EQ(seqs[0], 0U);
  EXPECT_EQ(seqs[1], 1U);
  EXPECT_EQ(seqs[2], 2U);
}

TEST(IntraBackendTest, ThreeReadersFanout) {
  tianshu::transport::intra::IntraBackend backend;
  ChannelConfig cfg;
  cfg.channel_name = "/test/three_readers";

  auto writer = backend.create_writer(cfg);
  auto r1 = backend.create_reader(cfg);
  auto r2 = backend.create_reader(cfg);
  auto r3 = backend.create_reader(cfg);

  std::atomic<int> c1{0}, c2{0}, c3{0};
  r1->set_callback([&](const Message&) { c1.fetch_add(1); });
  r2->set_callback([&](const Message&) { c2.fetch_add(1); });
  r3->set_callback([&](const Message&) { c3.fetch_add(1); });

  writer->write("broadcast3", 11);

  EXPECT_EQ(c1.load(), 1);
  EXPECT_EQ(c2.load(), 1);
  EXPECT_EQ(c3.load(), 1);
}

TEST(IntraBackendTest, EmptyMessage) {
  tianshu::transport::intra::IntraBackend backend;
  ChannelConfig cfg;
  cfg.channel_name = "/test/empty";

  auto writer = backend.create_writer(cfg);
  auto reader = backend.create_reader(cfg);

  std::atomic<int> count{0};
  std::size_t received_size = 999;
  reader->set_callback([&](const Message& msg) {
    count.fetch_add(1);
    received_size = msg.size;
  });

  writer->write(nullptr, 0);
  EXPECT_EQ(count.load(), 1);
  EXPECT_EQ(received_size, 0U);
}

}  // namespace
