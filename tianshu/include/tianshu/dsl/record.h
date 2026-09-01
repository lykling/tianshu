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

// Record substrate v0 (ADR-0026 Phase C): persist a flow's channel
// histories to a file and replay them through a fresh runtime — the
// same slice-query API over a file instead of memory. Replay
// re-publishes every recorded message, so lineage cascades rebuild
// exactly as they did live; the sink outputs must match bit-for-bit.
//
// Format (binary, fixed little-endian widths, append-only):
//   [u64 magic 'TREC0001'][u32 record_count]
//   per record:
//     [u32 channel_len][channel bytes]
//     [u64 seq][u32 payload_len][payload bytes]
//     [u32 lineage_len][lineage describe string bytes]

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tianshu::dsl {

struct RecordedMessage {
  std::string channel;
  std::uint64_t seq{0};
  std::vector<std::uint8_t> bytes;
  std::string lineage_text;
};

// Simple append-only record file (v0: single write at close).
class RecordFile {
 public:
  explicit RecordFile(std::string path);

  void append(const RecordedMessage& msg);
  [[nodiscard]] bool save() const;
  [[nodiscard]] static std::vector<RecordedMessage> load(const std::string& path);

  [[nodiscard]] std::size_t size() const { return records_.size(); }
  [[nodiscard]] const std::vector<RecordedMessage>& records() const { return records_; }

 private:
  std::string path_;
  std::vector<RecordedMessage> records_;
};

}  // namespace tianshu::dsl
