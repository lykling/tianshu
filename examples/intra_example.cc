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

// INTRA transport example: writer and reader in the same process.
// Demonstrates zero-copy same-process communication.

#include <iostream>
#include <string>
#include <vector>

#include "tianshu/core/node.h"
#include "tianshu/transport/transport_backend.h"

int main() {
  std::cout << "=== TIANSHU INTRA Transport Example ===\n";

  tianshu::core::Node node;

  auto writer = node.create_writer("/test/intra", "string");
  auto reader = node.create_reader("/test/intra", "string");

  std::vector<std::string> received;
  reader->set_callback([&](const tianshu::transport::Message& msg) {
    const std::string s(static_cast<const char*>(msg.data), msg.size);
    received.push_back(s);
    std::cout << "  [READ] seq=" << msg.seq << " data=" << s << "\n";
  });

  const std::vector<std::string> messages = {"hello", "world", "tianshu"};
  for (const auto& msg : messages) {
    std::cout << "  [WRITE] data=" << msg << "\n";
    writer->write(msg.data(), msg.size());
  }

  std::cout << "\nSent " << messages.size() << " messages, received " << received.size() << "\n";
  std::cout << "INTRA: zero-copy, no serialization, direct callback.\n";

  return received.size() == messages.size() ? 0 : 1;
}
