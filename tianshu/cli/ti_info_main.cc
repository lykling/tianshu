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

// ti-info: inspect a Tianshu Record v2 file (channels, statistics,
// metadata). Works on any .trec file without linking publisher code.
//
//   ti info <file.trec>                    summary
//   ti info <file.trec> --messages         dump first N messages per channel
//   ti info <file.trec> --lineage          show lineage for messages

#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <unordered_map>
#include <vector>

#include "tianshu/dsl/record_v2.h"

namespace {

void print_usage() {
  static_cast<void>(
      std::fprintf(stderr, "usage: ti-info <file.trec> [--messages N] [--lineage]\n"));
}

void dump_messages_preview(tianshu::dsl::record::RecordReader& reader,
                           std::uint64_t max_per_channel, bool show_lineage) {
  static_cast<void>(std::printf("\n--- messages (first %llu per channel) ---\n",
                                static_cast<unsigned long long>(max_per_channel)));
  std::unordered_map<std::uint16_t, std::uint64_t> per_channel_count;
  tianshu::dsl::record::RecordedMessageV2 msg;
  while (reader.next(&msg)) {
    const auto count_it = per_channel_count.find(msg.channel_id);
    const std::uint64_t count = count_it != per_channel_count.end() ? count_it->second : 0;
    if (count >= max_per_channel) {
      continue;
    }
    per_channel_count[msg.channel_id] = count + 1;
    const auto* ch = reader.find_channel(msg.channel_id);
    const std::string ch_name = ch != nullptr ? ch->name : "?";
    static_cast<void>(std::printf("  [%s] seq=%llu ts=%llu size=%zu", ch_name.c_str(),
                                  static_cast<unsigned long long>(msg.seq),
                                  static_cast<unsigned long long>(msg.ts_ns), msg.payload.size()));
    if (show_lineage && msg.lineage.has_value()) {
      static_cast<void>(std::printf(" lineage: %s", msg.lineage->describe().c_str()));
    }
    static_cast<void>(std::printf("\n"));
  }
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc < 2) {
    print_usage();
    return 2;
  }

  const std::string path = argv[1];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  std::uint64_t dump_messages = 0;
  bool show_lineage = false;
  for (int i = 2; i < argc; ++i) {
    const std::string arg(argv[i]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (arg == "--messages" && i + 1 < argc) {
      dump_messages = std::strtoull(argv[++i], nullptr, 10);  // NOLINT
    } else if (arg == "--lineage") {
      show_lineage = true;
    }
  }

  auto reader_opt = tianshu::dsl::record::RecordReader::open(path);
  if (!reader_opt.has_value()) {
    static_cast<void>(
        std::fprintf(stderr, "ti-info: cannot open %s (not a v2 record file?)\n", path.c_str()));
    return 1;
  }
  auto& reader = reader_opt.value();

  // Summary.
  const auto& stats = reader.stats();
  static_cast<void>(std::printf("file: %s\n", path.c_str()));
  static_cast<void>(std::printf("  version:  v%u\n", reader.major_version()));
  static_cast<void>(std::printf("  channels: %zu\n", reader.channels().size()));
  static_cast<void>(
      std::printf("  messages: %llu\n", static_cast<unsigned long long>(stats.total_messages)));
  static_cast<void>(
      std::printf("  bytes:    %llu\n", static_cast<unsigned long long>(stats.total_bytes)));
  if (stats.duration_ns > 0) {
    static_cast<void>(
        std::printf("  duration: %.3fs\n", static_cast<double>(stats.duration_ns) / 1e9));
  }
  static_cast<void>(std::printf("\n"));

  // Per-channel statistics.
  static_cast<void>(
      std::printf("%-30s %8s %12s %8s %10s\n", "CHANNEL", "COUNT", "BYTES", "TYPE", "RATE(hz)"));
  static_cast<void>(
      std::printf("%-30s %8s %12s %8s %10s\n", "-------", "-----", "-----", "----", "--------"));
  for (const auto& ch : reader.channels()) {
    static_cast<void>(std::printf("%-30s %8llu %12llu %-8s %10.1f\n", ch.name.c_str(),
                                  static_cast<unsigned long long>(ch.message_count),
                                  static_cast<unsigned long long>(ch.payload_bytes),
                                  ch.type_name.c_str(), ch.avg_rate_hz));
  }

  // Optional: dump messages.
  if (dump_messages > 0) {
    dump_messages_preview(reader, dump_messages, show_lineage);
  }

  return 0;
  return 0;
} catch (const std::exception& e) {
  static_cast<void>(std::fprintf(stderr, "ti-info: %s\n", e.what()));
  return 1;
}
