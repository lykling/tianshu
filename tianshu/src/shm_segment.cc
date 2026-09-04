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

#include "tianshu/shm/shm_segment.h"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <thread>

#include <sys/mman.h>
#include <sys/stat.h>

namespace tianshu::shm {

namespace {

constexpr int kOpenFlagsCreate = O_CREAT | O_EXCL | O_RDWR;
constexpr int kOpenFlagsExisting = O_RDWR;

std::size_t page_size() { return static_cast<std::size_t>(sysconf(_SC_PAGESIZE)); }

}  // namespace

// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)

std::unique_ptr<ShmSegment> ShmSegment::open_or_create(std::string_view name, std::size_t size) {
  auto segment = std::unique_ptr<ShmSegment>(new ShmSegment());
  if (!segment->init(name, size)) {
    return nullptr;
  }
  return segment;
}

std::unique_ptr<ShmSegment> ShmSegment::open_existing(std::string_view name) {
  auto segment = std::unique_ptr<ShmSegment>(new ShmSegment());
  if (name.empty() || name.size() >= sizeof(segment->name_)) {
    return nullptr;
  }
  std::memcpy(segment->name_, name.data(), name.size());
  if (!segment->attach_existing()) {
    return nullptr;
  }
  return segment;
}

ShmSegment::~ShmSegment() {
  if (base_ == nullptr) {
    return;
  }
  const bool last = header()->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1;
  munmap(base_, mapped_size_);
  if (last) {
    shm_unlink(name_);
  }
}

bool ShmSegment::init(std::string_view name, std::size_t size) {
  if (name.empty() || name.size() >= sizeof(name_) || size < page_size()) {
    return false;
  }
  std::memcpy(name_, name.data(), name.size());

  if (create_new(size + sizeof(SegmentHeader))) {
    return true;
  }
  if (errno != EEXIST) {
    return false;
  }
  return attach_existing();
}

bool ShmSegment::create_new(std::size_t total) {
  const int fd = shm_open(name_, kOpenFlagsCreate, 0600);
  if (fd < 0) {
    return false;
  }
  if (ftruncate(fd, static_cast<off_t>(total)) != 0 || !map_fd(fd, total)) {
    close(fd);
    shm_unlink(name_);
    return false;
  }
  close(fd);

  auto* hdr = header();
  hdr->size = total;
  hdr->refcount.store(1, std::memory_order_relaxed);
  hdr->magic.store(kMagic, std::memory_order_release);
  return true;
}

bool ShmSegment::attach_existing() {
  const int fd = shm_open(name_, kOpenFlagsExisting, 0600);
  if (fd < 0) {
    return false;
  }

  // The name exists from the creator's shm_open, but the file may not be
  // ftruncate'd yet — mapping a page of a zero-sized object is SIGBUS.
  // Wait for the header to become backed before probing it.
  bool sized = false;
  const auto header_bytes = static_cast<off_t>(sizeof(SegmentHeader));
  for (int i = 0; i < 1000 && !sized; ++i) {
    struct stat st{};
    sized = fstat(fd, &st) == 0 && st.st_size >= header_bytes;
    if (!sized) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  if (!sized) {
    close(fd);
    return false;
  }

  void* probe = mmap(nullptr, page_size(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (probe == MAP_FAILED) {
    close(fd);
    return false;
  }

  std::uint64_t total_size = 0;
  bool published = false;
  auto* probe_hdr = reinterpret_cast<SegmentHeader*>(probe);
  for (int i = 0; i < 1000; ++i) {
    if (probe_hdr->magic.load(std::memory_order_acquire) == kMagic) {
      published = true;
      total_size = probe_hdr->size;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  munmap(probe, page_size());

  if (!published || total_size < page_size()) {
    close(fd);
    return false;
  }

  if (!map_fd(fd, total_size)) {
    close(fd);
    return false;
  }
  close(fd);

  header()->refcount.fetch_add(1, std::memory_order_acq_rel);
  return true;
}

bool ShmSegment::map_fd(int fd, std::size_t size) {
  base_ = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (base_ == MAP_FAILED) {
    base_ = nullptr;
    return false;
  }
  mapped_size_ = size;
  return true;
}

// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

}  // namespace tianshu::shm
