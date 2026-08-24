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

// Unit tests for shm primitives: offset_ptr, ShmSegment, SpscRing (L4-TRANS-24, L4-TRANS-3).

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>

#include "tianshu/shm/offset_ptr.h"
#include "tianshu/shm/shm_ring.h"
#include "tianshu/shm/shm_segment.h"

namespace {

struct LinkedNode {
  int value{0};
  tianshu::shm::offset_ptr<LinkedNode> next;
};

TEST(OffsetPtrTest, DefaultIsNull) {
  const tianshu::shm::offset_ptr<int> p;
  EXPECT_FALSE(p);
  EXPECT_EQ(p.get(), nullptr);
}

TEST(OffsetPtrTest, RoundTripSameAddress) {
  int value = 42;
  tianshu::shm::offset_ptr<int> p(&value);
  ASSERT_TRUE(p);
  EXPECT_EQ(p.get(), &value);
  EXPECT_EQ(*p, 42);
}

TEST(OffsetPtrTest, LinkedNodesTraverse) {
  LinkedNode a;
  LinkedNode b;
  a.value = 1;
  b.value = 2;
  a.next = &b;
  ASSERT_TRUE(a.next);
  EXPECT_EQ(a.next->value, 2);
  EXPECT_EQ(a.next.get(), &b);
}

TEST(OffsetPtrTest, CopyPreservesTarget) {
  LinkedNode b;
  b.value = 7;
  LinkedNode a;
  a.next = &b;

  auto copied = a.next;
  EXPECT_EQ(copied.get(), &b);
  EXPECT_EQ(copied->value, 7);
}

TEST(OffsetPtrTest, MoveTransfersOwnership) {
  int value = 9;
  tianshu::shm::offset_ptr<int> p1(&value);
  auto p2 = std::move(p1);
  EXPECT_FALSE(p1);
  ASSERT_TRUE(p2);
  EXPECT_EQ(*p2, 9);
}

TEST(OffsetPtrTest, AssignNullptr) {
  int value = 1;
  tianshu::shm::offset_ptr<int> p(&value);
  p = nullptr;
  EXPECT_FALSE(p);
}

TEST(ShmSegmentTest, CreateWriteReadDestroy) {
  auto segment = tianshu::shm::ShmSegment::open_or_create("/tianshu_test_seg_a", 4096);
  ASSERT_NE(segment, nullptr);
  EXPECT_EQ(segment->size(), 4096U);

  auto* raw = static_cast<std::uint64_t*>(segment->data());
  raw[100] = 0xDEADBEEFCAFEBABEULL;
  EXPECT_EQ(raw[100], 0xDEADBEEFCAFEBABEULL);
}

TEST(ShmSegmentTest, AttachSeesData) {
  auto segment = tianshu::shm::ShmSegment::open_or_create("/tianshu_test_seg_b", 4096);
  ASSERT_NE(segment, nullptr);
  auto* raw = static_cast<std::uint64_t*>(segment->data());
  raw[200] = 123456789;

  auto segment2 = tianshu::shm::ShmSegment::open_or_create("/tianshu_test_seg_b", 4096);
  ASSERT_NE(segment2, nullptr);
  auto* raw2 = static_cast<std::uint64_t*>(segment2->data());
  EXPECT_EQ(raw2[200], 123456789U);
}

TEST(ShmSegmentTest, ConcurrentOpenBothSucceed) {
  auto s1 = tianshu::shm::ShmSegment::open_or_create("/tianshu_test_seg_c", 8192);
  auto s2 = tianshu::shm::ShmSegment::open_or_create("/tianshu_test_seg_c", 8192);
  ASSERT_NE(s1, nullptr);
  ASSERT_NE(s2, nullptr);

  auto* raw1 = static_cast<std::uint64_t*>(s1->data());
  auto* raw2 = static_cast<std::uint64_t*>(s2->data());
  raw1[300] = 777;
  EXPECT_EQ(raw2[300], 777U);
}

TEST(ShmSegmentTest, InvalidNameRejected) {
  EXPECT_EQ(tianshu::shm::ShmSegment::open_or_create("", 4096), nullptr);
}

// ENOENT: the name implies a missing directory under /dev/shm.
TEST(ShmSegmentTest, CreateFailsOnMissingDirectory) {
  EXPECT_EQ(tianshu::shm::ShmSegment::open_or_create("/tianshu_missing_dir/seg", 4096), nullptr);
}

// Bare segment (no published magic): attachers poll until timeout.
TEST(ShmSegmentTest, AttachTimesOutOnUnpublishedSegment) {
  const char* name = "/tianshu_test_bare";
  const int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(ftruncate(fd, 8192), 0);
  close(fd);

  EXPECT_EQ(tianshu::shm::ShmSegment::open_or_create(name, 4096), nullptr);
  shm_unlink(name);
}

// Forged header with absurd size: the full mmap must fail.
TEST(ShmSegmentTest, AttachFailsOnOversizedPublishedSize) {
  const char* name = "/tianshu_test_huge";
  const std::size_t total = sizeof(tianshu::shm::SegmentHeader) + 8192;
  const int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(ftruncate(fd, static_cast<off_t>(total)), 0);

  void* mem = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  ASSERT_NE(mem, MAP_FAILED);

  auto* hdr = static_cast<tianshu::shm::SegmentHeader*>(mem);
  hdr->size = 1ULL << 48;
  hdr->refcount.store(1, std::memory_order_relaxed);
  hdr->magic.store(tianshu::shm::ShmSegment::kMagic, std::memory_order_release);
  munmap(mem, total);

  EXPECT_EQ(tianshu::shm::ShmSegment::open_or_create(name, 4096), nullptr);
  shm_unlink(name);
}

#ifdef TIANSHU_HAVE_GCOV_DUMP
// Strong reference: a weak one stays unresolved because the linker never
// pulls archive members for weak undefined symbols.
extern "C" void __gcov_dump(void);
// Forked children must dump before _exit(); the rlimit this test sets would
// otherwise make the gcda write itself fail with EFBIG.
void dump_child_gcov() {
  rlimit lim{};
  lim.rlim_cur = RLIM_INFINITY;
  lim.rlim_max = RLIM_INFINITY;
  setrlimit(RLIMIT_FSIZE, &lim);
  __gcov_dump();
}
#else
void dump_child_gcov() {}
#endif

// ftruncate beyond RLIMIT_FSIZE; forked so the rlimit stays child-local.
TEST(ShmSegmentTest, CreateFailsUnderFsizeLimit) {
  const pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    std::signal(SIGXFSZ, SIG_IGN);
    rlimit lim{};
    getrlimit(RLIMIT_FSIZE, &lim);
    lim.rlim_cur = 4096;
    setrlimit(RLIMIT_FSIZE, &lim);
    const bool ok =
        tianshu::shm::ShmSegment::open_or_create("/tianshu_test_fsize", 1 << 20) != nullptr;
    shm_unlink("/tianshu_test_fsize");
    dump_child_gcov();
    _exit(ok ? 1 : 0);
  }
  int status = 0;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

constexpr std::size_t kRingCap = 4096;

TEST(SpscRingTest, PushPopRoundTrip) {
  std::vector<std::uint8_t> mem(tianshu::shm::SpscRing::total_size(kRingCap));
  auto ring = tianshu::shm::SpscRing::create(mem.data(), kRingCap);

  const std::string payload = "hello_shm";
  tianshu::shm::SpscRing::Metadata meta{42, 123456789};
  ASSERT_TRUE(ring.push(payload.data(), payload.size(), meta));

  std::vector<std::uint8_t> out;
  tianshu::shm::SpscRing::Metadata got{};
  ASSERT_TRUE(ring.pop(&out, &got));
  EXPECT_EQ(out.size(), payload.size());
  EXPECT_EQ(std::string(out.begin(), out.end()), payload);
  EXPECT_EQ(got.seq, 42U);
  EXPECT_EQ(got.timestamp_ns, 123456789);
  EXPECT_TRUE(ring.empty());
}

TEST(SpscRingTest, PopEmptyReturnsFalse) {
  std::vector<std::uint8_t> mem(tianshu::shm::SpscRing::total_size(kRingCap));
  auto ring = tianshu::shm::SpscRing::create(mem.data(), kRingCap);

  std::vector<std::uint8_t> out;
  tianshu::shm::SpscRing::Metadata meta{};
  EXPECT_FALSE(ring.pop(&out, &meta));
}

TEST(SpscRingTest, FifoOrder) {
  std::vector<std::uint8_t> mem(tianshu::shm::SpscRing::total_size(kRingCap));
  auto ring = tianshu::shm::SpscRing::create(mem.data(), kRingCap);

  for (int i = 0; i < 10; ++i) {
    const std::uint32_t v = static_cast<std::uint32_t>(i);
    ASSERT_TRUE(ring.push(&v, sizeof(v), {static_cast<std::uint64_t>(i), 0}));
  }

  std::vector<std::uint8_t> out;
  tianshu::shm::SpscRing::Metadata meta{};
  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(ring.pop(&out, &meta));
    ASSERT_EQ(out.size(), sizeof(std::uint32_t));
    std::uint32_t v = 0;
    std::memcpy(&v, out.data(), sizeof(v));
    EXPECT_EQ(v, static_cast<std::uint32_t>(i));
    EXPECT_EQ(meta.seq, static_cast<std::uint64_t>(i));
  }
  EXPECT_TRUE(ring.empty());
}

TEST(SpscRingTest, FullDropsNewMessage) {
  const std::size_t small_cap = 128;
  std::vector<std::uint8_t> mem(tianshu::shm::SpscRing::total_size(small_cap));
  auto ring = tianshu::shm::SpscRing::create(mem.data(), small_cap);

  const std::string blob(100, 'x');
  ASSERT_TRUE(ring.push(blob.data(), blob.size(), {1, 1}));
  EXPECT_FALSE(ring.push(blob.data(), blob.size(), {2, 2}));

  std::vector<std::uint8_t> out;
  tianshu::shm::SpscRing::Metadata meta{};
  ASSERT_TRUE(ring.pop(&out, &meta));
  EXPECT_EQ(meta.seq, 1U);
  EXPECT_FALSE(ring.pop(&out, &meta));
}

TEST(SpscRingTest, WrapAroundKeepsOrder) {
  const std::size_t cap = 256;
  std::vector<std::uint8_t> mem(tianshu::shm::SpscRing::total_size(cap));
  auto ring = tianshu::shm::SpscRing::create(mem.data(), cap);

  for (int round = 0; round < 20; ++round) {
    const std::string msg(60, static_cast<char>('a' + round));
    ASSERT_TRUE(ring.push(msg.data(), msg.size(), {static_cast<std::uint64_t>(round), 0}))
        << "round " << round;

    std::vector<std::uint8_t> out;
    tianshu::shm::SpscRing::Metadata meta{};
    ASSERT_TRUE(ring.pop(&out, &meta)) << "round " << round;
    EXPECT_EQ(std::string(out.begin(), out.end()), msg);
    EXPECT_EQ(meta.seq, static_cast<std::uint64_t>(round));
  }
}

TEST(SpscRingTest, ConcurrentProducerConsumer) {
  constexpr int kMessages = 20000;
  std::vector<std::uint8_t> mem(tianshu::shm::SpscRing::total_size(kRingCap));
  auto ring = tianshu::shm::SpscRing::create(mem.data(), kRingCap);

  std::atomic<int> consumed{0};
  std::atomic<bool> producer_done{false};

  std::thread producer([&]() {
    for (int i = 0; i < kMessages; ++i) {
      const std::uint32_t v = static_cast<std::uint32_t>(i);
      while (!ring.push(&v, sizeof(v), {static_cast<std::uint64_t>(i), 0})) {
        std::this_thread::yield();
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::thread consumer([&]() {
    std::vector<std::uint8_t> out;
    tianshu::shm::SpscRing::Metadata meta{};
    while (consumed.load() < kMessages) {
      if (ring.pop(&out, &meta)) {
        consumed.fetch_add(1, std::memory_order_relaxed);
      } else if (producer_done.load(std::memory_order_acquire)) {
        if (!ring.pop(&out, &meta)) {
          break;
        }
        consumed.fetch_add(1, std::memory_order_relaxed);
      } else {
        std::this_thread::yield();
      }
    }
  });

  producer.join();
  consumer.join();
  EXPECT_EQ(consumed.load(), kMessages);
  EXPECT_TRUE(ring.empty());
}

}  // namespace
