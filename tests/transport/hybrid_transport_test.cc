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

// Unit tests for HybridTransport (L4-TRANS-19).

#include "tianshu/transport/hybrid_transport.h"

#include <atomic>
#include <string>

#include <gtest/gtest.h>

#include "tianshu/transport/transport_backend.h"

namespace {

using tianshu::transport::BackendType;
using tianshu::transport::ChannelConfig;
using tianshu::transport::Message;
using tianshu::transport::TransportBackend;

TEST(HybridTransportTest, TypeIsIntra) {
  const tianshu::transport::HybridTransport ht;
  EXPECT_EQ(ht.type(), BackendType::kIntra);
}

TEST(HybridTransportTest, SupportsZeroCopy) {
  const tianshu::transport::HybridTransport ht;
  EXPECT_TRUE(ht.supports_zero_copy());
}

TEST(HybridTransportTest, DoesNotSupportRemote) {
  const tianshu::transport::HybridTransport ht;
  EXPECT_FALSE(ht.supports_remote());
}

TEST(HybridTransportTest, CreateWriterAndReader) {
  tianshu::transport::HybridTransport ht;
  ChannelConfig cfg;
  cfg.channel_name = "/hybrid/test";

  auto writer = ht.create_writer(cfg);
  auto reader = ht.create_reader(cfg);
  ASSERT_NE(writer, nullptr);
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(writer->channel(), "/hybrid/test");
  EXPECT_EQ(reader->channel(), "/hybrid/test");
}

TEST(HybridTransportTest, EndToEndCommunication) {
  tianshu::transport::HybridTransport ht;
  ChannelConfig cfg;
  cfg.channel_name = "/hybrid/e2e";

  auto writer = ht.create_writer(cfg);
  auto reader = ht.create_reader(cfg);

  std::atomic<int> count{0};
  std::string received;
  reader->set_callback([&](const Message& msg) {
    count.fetch_add(1);
    received.assign(static_cast<const char*>(msg.data), msg.size);
  });

  std::string payload = "hybrid_msg";
  writer->write(payload.data(), payload.size());

  EXPECT_EQ(count.load(), 1);
  EXPECT_EQ(received, payload);
}

TEST(HybridTransportTest, PolymorphicAccess) {
  std::unique_ptr<TransportBackend> backend =
      std::make_unique<tianshu::transport::HybridTransport>();
  EXPECT_EQ(backend->type(), BackendType::kIntra);
  EXPECT_TRUE(backend->supports_zero_copy());
  EXPECT_FALSE(backend->supports_remote());

  ChannelConfig cfg;
  cfg.channel_name = "/poly/test";
  auto writer = backend->create_writer(cfg);
  ASSERT_NE(writer, nullptr);
}

}  // namespace
