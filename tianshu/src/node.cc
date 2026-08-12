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

#include "tianshu/core/node.h"

#include <memory>
#include <string>
#include <string_view>

#include "tianshu/transport/hybrid_transport.h"
#include "tianshu/transport/transport_backend.h"

namespace tianshu::core {

Node::Node() : transport_(std::make_unique<transport::HybridTransport>()) {}

std::unique_ptr<transport::ReaderBase> Node::create_reader(std::string_view channel,
                                                           std::string_view msg_type) {
  transport::ChannelConfig cfg;
  cfg.channel_name = std::string(channel);
  cfg.msg_type_name = std::string(msg_type);
  return transport_->create_reader(cfg);
}

std::unique_ptr<transport::WriterBase> Node::create_writer(std::string_view channel,
                                                           std::string_view msg_type) {
  transport::ChannelConfig cfg;
  cfg.channel_name = std::string(channel);
  cfg.msg_type_name = std::string(msg_type);
  return transport_->create_writer(cfg);
}

}  // namespace tianshu::core
