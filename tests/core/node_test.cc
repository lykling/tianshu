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

// Unit tests for Node (L4-CORE-4).

#include "tianshu/core/node.h"

#include <atomic>
#include <string>

#include <gtest/gtest.h>

#include "tianshu/transport/transport_backend.h"

namespace {

using tianshu::transport::Message;

TEST(NodeTest, CreateReaderAndWriter) {
  tianshu::core::Node node;
  auto writer = node.create_writer("/node/test", "string");
  auto reader = node.create_reader("/node/test", "string");

  ASSERT_NE(writer, nullptr);
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(writer->channel(), "/node/test");
  EXPECT_EQ(reader->channel(), "/node/test");
}

TEST(NodeTest, CreateWriterOnly) {
  tianshu::core::Node node;
  auto writer = node.create_writer("/node/wonly", "string");
  ASSERT_NE(writer, nullptr);
  EXPECT_EQ(writer->channel(), "/node/wonly");
}

TEST(NodeTest, CreateReaderOnly) {
  tianshu::core::Node node;
  auto reader = node.create_reader("/node/ronly", "string");
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(reader->channel(), "/node/ronly");
}

TEST(NodeTest, CreateWriterWithoutMsgType) {
  tianshu::core::Node node;
  auto writer = node.create_writer("/node/no_type");
  ASSERT_NE(writer, nullptr);
  EXPECT_EQ(writer->channel(), "/node/no_type");
}

TEST(NodeTest, CreateReaderWithoutMsgType) {
  tianshu::core::Node node;
  auto reader = node.create_reader("/node/no_type");
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(reader->channel(), "/node/no_type");
}

TEST(NodeTest, IntraCommunication) {
  tianshu::core::Node node;
  auto writer = node.create_writer("/node/intra", "string");
  auto reader = node.create_reader("/node/intra", "string");

  std::atomic<int> count{0};
  std::string received;

  reader->set_callback([&](const Message& msg) {
    count.fetch_add(1);
    received.assign(static_cast<const char*>(msg.data), msg.size);
  });

  std::string test_msg = "node_test_msg";
  writer->write(test_msg.data(), test_msg.size());

  EXPECT_EQ(count.load(), 1);
  EXPECT_EQ(received, test_msg);
}

TEST(NodeTest, MultipleChannels) {
  tianshu::core::Node node;
  auto w1 = node.create_writer("/ch1");
  auto r1 = node.create_reader("/ch1");
  auto w2 = node.create_writer("/ch2");
  auto r2 = node.create_reader("/ch2");

  std::atomic<int> c1{0};
  std::atomic<int> c2{0};

  r1->set_callback([&](const Message&) { c1.fetch_add(1); });
  r2->set_callback([&](const Message&) { c2.fetch_add(1); });

  w1->write("a", 1);
  w2->write("b", 1);

  EXPECT_EQ(c1.load(), 1);
  EXPECT_EQ(c2.load(), 1);
}

TEST(NodeTest, WriterThenReaderCommunicates) {
  tianshu::core::Node node;
  auto writer = node.create_writer("/node/wtr", "string");
  auto reader = node.create_reader("/node/wtr", "string");

  std::atomic<int> count{0};
  reader->set_callback([&](const Message&) { count.fetch_add(1); });

  writer->write("after", 6);
  EXPECT_EQ(count.load(), 1);
}

TEST(NodeTest, MultipleNodesSameChannel) {
  // Different Node instances in the same process should still communicate
  // via the IntraChannelRegistry singleton.
  tianshu::core::Node node1;
  tianshu::core::Node node2;

  auto writer = node1.create_writer("/shared/ch", "string");
  auto reader = node2.create_reader("/shared/ch", "string");

  std::atomic<int> count{0};
  reader->set_callback([&](const Message&) { count.fetch_add(1); });

  writer->write("cross_node", 10);
  EXPECT_EQ(count.load(), 1);
}

}  // namespace
