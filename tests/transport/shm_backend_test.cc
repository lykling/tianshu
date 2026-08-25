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

// Tests for SHM transport backend (L4-TRANS-3/4), including a fork-based
// cross-process end-to-end case.

#include "tianshu/transport/shm_backend.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <sys/wait.h>

#include "tianshu/core/node.h"
#include "tianshu/transport/transport_backend.h"

namespace {

using tianshu::transport::BackendType;
using tianshu::transport::ChannelConfig;
using tianshu::transport::Message;

#ifdef TIANSHU_HAVE_GCOV_DUMP
extern "C" void __gcov_dump(void);
#else
#ifdef TIANSHU_HAVE_LLVM_PROFILE_WRITE
extern "C" void __llvm_profile_write_file(void);
extern "C" void __llvm_profile_set_filename(const char*);
#endif
#endif

// _exit() skips atexit handlers; forked children flush profiles by hand.
// Under LLVM the runtime resolved LLVM_PROFILE_FILE (%p) before fork, so
// the child must re-point at its own file first or the parent's exit dump
// overwrites it.
void dump_child_coverage() {
#ifdef TIANSHU_HAVE_GCOV_DUMP
  __gcov_dump();
#else
#ifdef TIANSHU_HAVE_LLVM_PROFILE_WRITE
  if (const char* env = getenv("LLVM_PROFILE_FILE")) {
    std::string dir(env);
    const auto slash = dir.rfind('/');
    dir = slash == std::string::npos ? "." : dir.substr(0, slash);
    char path[256];
    std::snprintf(path, sizeof(path), "%s/child_%%m_%d.profraw", dir.c_str(),
                  static_cast<int>(getpid()));
    __llvm_profile_set_filename(path);
  }
  __llvm_profile_write_file();
#endif
#endif
}

struct TestPacket {
  std::uint64_t seq;
  double value;
};

TEST(ShmBackendTest, TypeIsShm) {
  const tianshu::transport::shm::ShmBackend backend;
  EXPECT_EQ(backend.type(), BackendType::kShm);
  EXPECT_FALSE(backend.supports_zero_copy());
  EXPECT_FALSE(backend.supports_remote());
}

TEST(ShmBackendTest, EndpointsReportChannel) {
  tianshu::transport::shm::ShmBackend backend;

  ChannelConfig cfg;
  cfg.channel_name = "/shm/channel_names";

  auto reader = backend.create_reader(cfg);
  auto writer = backend.create_writer(cfg);
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(writer, nullptr);
  EXPECT_EQ(writer->channel(), "/shm/channel_names");
  EXPECT_EQ(reader->channel(), "/shm/channel_names");
}

TEST(ShmBackendTest, SlotExhaustionBeyondMaxReaders) {
  tianshu::transport::shm::ShmBackend backend;

  ChannelConfig cfg;
  cfg.channel_name = "/shm/slot_exhaustion";

  std::vector<std::unique_ptr<tianshu::transport::ReaderBase>> readers;
  for (std::uint32_t i = 0; i < tianshu::transport::shm::kMaxReaders; ++i) {
    auto reader = backend.create_reader(cfg);
    ASSERT_NE(reader, nullptr) << "reader " << i;
    readers.push_back(std::move(reader));
  }
  EXPECT_EQ(backend.create_reader(cfg), nullptr);

  readers.clear();
  auto revived = backend.create_reader(cfg);
  ASSERT_NE(revived, nullptr);
}

TEST(ShmBackendTest, RegistryExpiredEntryReopened) {
  tianshu::transport::shm::ShmBackend backend;

  ChannelConfig cfg;
  cfg.channel_name = "/shm/registry_reopen";
  {
    auto writer = backend.create_writer(cfg);
    auto reader = backend.create_reader(cfg);
    ASSERT_NE(writer, nullptr);
    ASSERT_NE(reader, nullptr);
  }

  auto reader2 = backend.create_reader(cfg);
  ASSERT_NE(reader2, nullptr);
}

TEST(ShmBackendTest, SameProcessWriterReader) {
  tianshu::transport::shm::ShmBackend backend;

  ChannelConfig cfg;
  cfg.channel_name = "/shm/same_process";

  auto reader = backend.create_reader(cfg);
  auto writer = backend.create_writer(cfg);
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(writer, nullptr);

  std::atomic<int> count{0};
  std::uint64_t last_seq = 0;
  double last_value = 0.0;

  reader->set_callback([&](const Message& msg) {
    TestPacket pkt{};
    std::memcpy(&pkt, msg.data, sizeof(pkt));
    last_seq = pkt.seq;
    last_value = pkt.value;
    count.fetch_add(1);
  });

  for (int i = 0; i < 50; ++i) {
    const TestPacket pkt{.seq = static_cast<std::uint64_t>(i), .value = i * 0.5};
    writer->write(&pkt, sizeof(pkt));
  }

  for (int spin = 0; spin < 2000 && count.load() < 50; ++spin) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(count.load(), 50);
  EXPECT_EQ(last_seq, 49U);
  EXPECT_DOUBLE_EQ(last_value, 24.5);
}

TEST(ShmBackendTest, MultipleReadersFanout) {
  tianshu::transport::shm::ShmBackend backend;

  ChannelConfig cfg;
  cfg.channel_name = "/shm/fanout";

  auto reader1 = backend.create_reader(cfg);
  auto reader2 = backend.create_reader(cfg);
  auto writer = backend.create_writer(cfg);
  ASSERT_NE(reader1, nullptr);
  ASSERT_NE(reader2, nullptr);
  ASSERT_NE(writer, nullptr);

  std::atomic<int> count1{0};
  std::atomic<int> count2{0};
  reader1->set_callback([&](const Message&) { count1.fetch_add(1); });
  reader2->set_callback([&](const Message&) { count2.fetch_add(1); });

  const TestPacket pkt{.seq = 1, .value = 1.0};
  writer->write(&pkt, sizeof(pkt));

  for (int spin = 0; spin < 2000 && (count1.load() < 1 || count2.load() < 1); ++spin) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(count1.load(), 1);
  EXPECT_EQ(count2.load(), 1);
}

TEST(ShmBackendTest, NodeShmModeEndToEnd) {
  tianshu::core::Node writer_node(tianshu::transport::TransportMode::kShm);
  tianshu::core::Node reader_node(tianshu::transport::TransportMode::kShm);

  auto writer = writer_node.create_writer("/shm/node_e2e");
  auto reader = reader_node.create_reader("/shm/node_e2e");
  ASSERT_NE(writer, nullptr);
  ASSERT_NE(reader, nullptr);

  std::atomic<int> count{0};
  reader->set_callback([&](const Message& msg) {
    ASSERT_EQ(msg.size, sizeof(TestPacket));
    count.fetch_add(1);
  });

  const TestPacket pkt{.seq = 77, .value = 3.25};
  writer->write(&pkt, sizeof(pkt));

  for (int spin = 0; spin < 2000 && count.load() < 1; ++spin) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(count.load(), 1);
}

// Fork-based cross-process test: child process reads, parent process writes.
// A pipe handshake ensures the child's reader is registered before the
// parent starts writing.
TEST(ShmBackendTest, CrossProcessEndToEnd) {
  const std::string channel = "/shm/cross_process";
  constexpr int kMessages = 100;

  int handshake_pipe[2];
  ASSERT_EQ(pipe(handshake_pipe), 0);

  const pid_t pid = fork();  // NOLINT(misc-include-cleaner)  // glibc: bits/types + sys/types
  ASSERT_GE(pid, 0);

  if (pid == 0) {
    // Child: reader.
    close(handshake_pipe[0]);

    int exit_code = 0;
    {
      tianshu::transport::shm::ShmBackend backend;
      ChannelConfig cfg;
      cfg.channel_name = channel;

      auto reader = backend.create_reader(cfg);
      if (reader == nullptr) {
        dump_child_coverage();
        _exit(10);
      }

      std::atomic<std::uint64_t> received{0};

      reader->set_callback([&](const Message& msg) {
        TestPacket pkt{};
        std::memcpy(&pkt, msg.data, msg.size);
        if (pkt.seq == static_cast<std::uint64_t>(received.load())) {
          received.fetch_add(1);
        }
      });

      const char ready = 'r';
      if (write(handshake_pipe[1], &ready, 1) != 1) {
        dump_child_coverage();
        _exit(11);
      }

      for (int spin = 0; spin < 5000 && received.load() < kMessages; ++spin) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }

      exit_code = received.load() == kMessages ? 0 : 20;
      // _exit() below skips destructors; scope exit releases slot + refcount.
    }
    dump_child_coverage();
    _exit(exit_code);
  }

  // Parent: writer.
  close(handshake_pipe[1]);

  char ready = 0;
  ASSERT_EQ(read(handshake_pipe[0], &ready, 1), 1);
  close(handshake_pipe[0]);
  ASSERT_EQ(ready, 'r');

  tianshu::transport::shm::ShmBackend backend;
  ChannelConfig cfg;
  cfg.channel_name = channel;

  auto writer = backend.create_writer(cfg);
  ASSERT_NE(writer, nullptr);

  for (int i = 0; i < kMessages; ++i) {
    const TestPacket pkt{.seq = static_cast<std::uint64_t>(i), .value = i * 1.5};
    writer->write(&pkt, sizeof(pkt));
  }

  int status = 0;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  EXPECT_TRUE(WIFEXITED(status));     // NOLINT(misc-include-cleaner)  // glibc: bits/waitflags
  EXPECT_EQ(WEXITSTATUS(status), 0);  // NOLINT(misc-include-cleaner)  // glibc: bits/waitflags
}

}  // namespace
