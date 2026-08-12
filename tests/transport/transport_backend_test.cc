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

// Unit tests for TransportRegistry (L4-TRANS-18).

#include "tianshu/transport/transport_backend.h"

#include <memory>

#include <gtest/gtest.h>

#include "tianshu/transport/intra_backend.h"

namespace {

TEST(TransportRegistryTest, RegisterAndGetIntra) {
  auto& registry = tianshu::transport::TransportRegistry::instance();
  registry.register_backend(tianshu::transport::BackendType::kIntra,
                            []() -> std::unique_ptr<tianshu::transport::TransportBackend> {
                              return std::make_unique<tianshu::transport::intra::IntraBackend>();
                            });

  auto* backend = registry.get(tianshu::transport::BackendType::kIntra);
  ASSERT_NE(backend, nullptr);
  EXPECT_EQ(backend->type(), tianshu::transport::BackendType::kIntra);
  EXPECT_TRUE(backend->supports_zero_copy());
  EXPECT_FALSE(backend->supports_remote());
}

TEST(TransportRegistryTest, GetUnregisteredReturnsNull) {
  auto& registry = tianshu::transport::TransportRegistry::instance();
  auto* backend = registry.get(tianshu::transport::BackendType::kZenoh);
  EXPECT_EQ(backend, nullptr);
}

TEST(TransportRegistryTest, RegisteredBackendCreatesWriter) {
  auto& registry = tianshu::transport::TransportRegistry::instance();
  registry.register_backend(tianshu::transport::BackendType::kIntra,
                            []() -> std::unique_ptr<tianshu::transport::TransportBackend> {
                              return std::make_unique<tianshu::transport::intra::IntraBackend>();
                            });

  auto* backend = registry.get(tianshu::transport::BackendType::kIntra);
  ASSERT_NE(backend, nullptr);

  tianshu::transport::ChannelConfig cfg;
  cfg.channel_name = "/registry/test";
  auto writer = backend->create_writer(cfg);
  ASSERT_NE(writer, nullptr);
  EXPECT_EQ(writer->channel(), "/registry/test");
}

TEST(TransportRegistryTest, RegisteredBackendCreatesReader) {
  auto& registry = tianshu::transport::TransportRegistry::instance();
  registry.register_backend(tianshu::transport::BackendType::kIntra,
                            []() -> std::unique_ptr<tianshu::transport::TransportBackend> {
                              return std::make_unique<tianshu::transport::intra::IntraBackend>();
                            });

  auto* backend = registry.get(tianshu::transport::BackendType::kIntra);
  ASSERT_NE(backend, nullptr);

  tianshu::transport::ChannelConfig cfg;
  cfg.channel_name = "/registry/reader";
  auto reader = backend->create_reader(cfg);
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(reader->channel(), "/registry/reader");
}

}  // namespace
