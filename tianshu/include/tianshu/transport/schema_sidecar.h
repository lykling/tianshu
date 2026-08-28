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

// Channel schema sidecar (ADR-0020 Phase 2, SHM option b):
//   - the writer publishes the channel's schema blob into a small sidecar
//     segment next to the ring-buffer segment, so receivers (ti-monitor)
//     can decode payloads without linking the publisher's types
//   - sidecar payload: [u64 magic (release)][u32 blob_len][blob bytes]
//   - zero changes to the existing channel segment layout; one page per
//     schema-carrying channel

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tianshu::transport::shm {

// Ring-buffer segment name for a channel (FNV-1a of the channel name).
[[nodiscard]] std::string segment_name_for(std::string_view channel);

// Sidecar segment name for a channel (same hash, different prefix).
[[nodiscard]] std::string schema_segment_name_for(std::string_view channel);

// Publishes the schema blob beside the channel. Idempotent (first writer
// wins); keeps the sidecar mapped until process exit so it outlives the
// WriterBase that published it. No-op when the blob exceeds one page.
void write_channel_schema(std::string_view channel, const std::uint8_t* blob, std::size_t size);

// Reads the published schema blob; false when the channel has no sidecar
// (untyped publisher) or the payload is malformed/truncated.
[[nodiscard]] bool read_channel_schema(std::string_view channel, std::vector<std::uint8_t>* blob);

}  // namespace tianshu::transport::shm
