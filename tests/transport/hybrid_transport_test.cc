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

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <sys/wait.h>

#include "tianshu/transport/intra_backend.h"
#include "tianshu/transport/shm_backend.h"
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

TEST(HybridTransportTest, ShmModeRoutesToShmBackend) {
  tianshu::transport::HybridTransport ht(tianshu::transport::TransportMode::kShm);
  EXPECT_EQ(ht.type(), BackendType::kShm);
  EXPECT_FALSE(ht.supports_zero_copy());

  ChannelConfig cfg;
  cfg.channel_name = "/hybrid/shm_mode";
  auto writer = ht.create_writer(cfg);
  ASSERT_NE(writer, nullptr);
  EXPECT_EQ(writer->channel(), "/hybrid/shm_mode");
}

TEST(HybridTransportTest, AutoModeFallsBackToIntra) {
  tianshu::transport::HybridTransport ht(tianshu::transport::TransportMode::kAuto);
  EXPECT_EQ(ht.type(), BackendType::kIntra);
  EXPECT_TRUE(ht.supports_zero_copy());

  ChannelConfig cfg;
  cfg.channel_name = "/hybrid/auto_mode";
  auto writer = ht.create_writer(cfg);
  auto reader = ht.create_reader(cfg);
  ASSERT_NE(writer, nullptr);
  ASSERT_NE(reader, nullptr);
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

// ---------------------------------------------------------------------------
// kAuto (L4-TRANS-21, ADR-0023)
// ---------------------------------------------------------------------------

TEST(HybridTransportTest, AutoSameProcessPrefersIntra) {
  tianshu::transport::HybridTransport ht(tianshu::transport::TransportMode::kAuto);
  ChannelConfig cfg;
  cfg.channel_name = "/hybrid/auto/intra";

  auto writer = ht.create_writer(cfg);
  ASSERT_NE(writer, nullptr);
  auto reader = ht.create_reader(cfg);
  ASSERT_NE(reader, nullptr);

  EXPECT_NE(dynamic_cast<tianshu::transport::intra::IntraReader*>(reader.get()), nullptr);

  std::atomic<int> got{0};
  reader->set_callback([&got](const Message&) { got.fetch_add(1); });
  const int v = 7;
  writer->write(&v, sizeof(v));
  // INTRA is synchronous: the callback fires inside write().
  EXPECT_GE(got.load(), 1);
}

TEST(HybridTransportTest, AutoReaderBeforeWriterStillServed) {
  tianshu::transport::HybridTransport ht(tianshu::transport::TransportMode::kAuto);
  ChannelConfig cfg;
  cfg.channel_name = "/hybrid/auto/late";

  // Reader first: no local writer yet -> SHM reader.
  auto reader = ht.create_reader(cfg);
  ASSERT_NE(reader, nullptr);
  EXPECT_NE(dynamic_cast<tianshu::transport::shm::ShmReader*>(reader.get()), nullptr);

  std::atomic<int> got{0};
  reader->set_callback([&got](const Message&) { got.fetch_add(1); });

  // Late local writer dual-publishes; the earlier SHM reader is served.
  auto writer = ht.create_writer(cfg);
  ASSERT_NE(writer, nullptr);
  const int v = 3;
  writer->write(&v, sizeof(v));
  for (int i = 0; i < 100 && got.load() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_GE(got.load(), 1);
}

TEST(HybridTransportTest, AutoCrossProcessFallsBackToShm) {
  const char* channel = "/hybrid/auto/cross";
  const pid_t pid = fork();  // NOLINT(misc-include-cleaner)
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    tianshu::transport::HybridTransport ht(tianshu::transport::TransportMode::kAuto);
    ChannelConfig cfg;
    cfg.channel_name = channel;
    auto writer = ht.create_writer(cfg);
    if (writer == nullptr) {
      _exit(10);
    }
    for (int i = 0; i < 200; ++i) {
      const int v = i;
      writer->write(&v, sizeof(v));
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    _exit(0);
  }

  // Let the publisher create the channel first; no local writer exists in
  // this process, so the kAuto reader attaches SHM.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  tianshu::transport::HybridTransport ht(tianshu::transport::TransportMode::kAuto);
  ChannelConfig cfg;
  cfg.channel_name = channel;
  auto reader = ht.create_reader(cfg);
  ASSERT_NE(reader, nullptr);
  EXPECT_NE(dynamic_cast<tianshu::transport::shm::ShmReader*>(reader.get()), nullptr);

  std::atomic<int> got{0};
  reader->set_callback([&got](const Message&) { got.fetch_add(1); });
  for (int i = 0; i < 200 && got.load() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_GE(got.load(), 1);

  int status = 0;
  kill(pid, SIGTERM);  // NOLINT(misc-include-cleaner)
  waitpid(pid, &status, 0);
}

TEST(HybridTransportTest, AutoRegistryIgnoresPhantomWriters) {
  auto& reg = tianshu::transport::intra::IntraChannelRegistry::instance();
  EXPECT_FALSE(reg.has_writer("/hybrid/auto/phantom"));
  {
    // A reader-only channel creates a registry entry (phantom) that must
    // NOT count as a real publisher.
    tianshu::transport::HybridTransport ht(tianshu::transport::TransportMode::kIntra);
    ChannelConfig cfg;
    cfg.channel_name = "/hybrid/auto/phantom";
    auto reader = ht.create_reader(cfg);
    ASSERT_NE(reader, nullptr);
  }
  EXPECT_FALSE(reg.has_writer("/hybrid/auto/phantom"));
}
