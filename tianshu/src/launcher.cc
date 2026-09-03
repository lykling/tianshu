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

#include "tianshu/core/launcher.h"

#include <unistd.h>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tianshu/core/node.h"
#include "tianshu/transport/transport_backend.h"

namespace tianshu::core {

namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

std::vector<std::string> split_inputs(std::string_view value) {
  std::vector<std::string> out;
  std::size_t pos = 0;
  while (pos < value.size()) {
    const std::size_t comma = value.find(',', pos);
    const auto part = trim(
        value.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos));
    if (!part.empty()) {
      out.emplace_back(part);
    }
    if (comma == std::string_view::npos) {
      break;
    }
    pos = comma + 1;
  }
  return out;
}

// Returns false on a malformed section header (error set on `result`).
bool parse_section_header(std::string_view line, DagParseResult* result,
                          DagComponentConfig** current) {
  const std::size_t close = line.find(']');
  if (close == std::string_view::npos || !line.starts_with("[component ")) {
    result->error = "unrecognized section: " + std::string(line);
    return false;
  }
  const auto name = trim(line.substr(11, close - 11));
  if (name.empty()) {
    result->error = "component section without a name";
    return false;
  }
  DagComponentConfig section{};
  section.name = std::string(name);
  result->components.push_back(std::move(section));
  *current = &result->components.back();
  return true;
}

// Returns false on an unknown key or malformed value (error set on `result`).
bool parse_key_value(std::string_view line, DagParseResult* result, DagComponentConfig* current) {
  const std::size_t eq = line.find('=');
  if (eq == std::string_view::npos || current == nullptr) {
    result->error = "key outside any [component] section: " + std::string(line);
    return false;
  }
  const auto key = trim(line.substr(0, eq));
  const auto value = trim(line.substr(eq + 1));

  if (key == "type") {
    current->type = std::string(value);
    return true;
  }
  if (key == "inputs") {
    current->input_channels = split_inputs(value);
    return true;
  }
  if (key == "interval_ms") {
    char* end = nullptr;
    const std::string number(value);
    const std::int64_t ms = std::strtoll(number.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || ms < 0) {
      result->error = "bad interval_ms: " + number;
      return false;
    }
    current->interval = std::chrono::milliseconds(ms);
    return true;
  }
  result->error = "unknown key: " + std::string(key);
  return false;
}

}  // namespace

DagParseResult DagConfig::parse(std::string_view text) {
  DagParseResult result;
  DagComponentConfig* current = nullptr;

  std::size_t line_start = 0;
  while (line_start <= text.size()) {
    const std::size_t line_end = text.find('\n', line_start);
    const auto raw_line =
        text.substr(line_start, line_end == std::string_view::npos ? std::string_view::npos
                                                                   : line_end - line_start);
    line_start = (line_end == std::string_view::npos) ? text.size() + 1 : line_end + 1;

    const auto line = trim(raw_line);
    if (line.empty() || line.front() == '#' || line.front() == ';') {
      continue;
    }
    if (line.front() == '[') {
      if (!parse_section_header(line, &result, &current)) {
        return result;
      }
      continue;
    }
    if (!parse_key_value(line, &result, current)) {
      return result;
    }
  }

  for (const auto& comp : result.components) {
    if (comp.type.empty()) {
      result.error = "component '" + comp.name + "' missing type";
      return result;
    }
  }
  return result;
}

DagParseResult DagConfig::parse_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    DagParseResult result;
    result.error = "cannot open " + path;
    return result;
  }
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return parse(text);
}

namespace {

volatile std::sig_atomic_t g_launch_stop = 0;

void launch_signal_handler(int /*sig*/) { g_launch_stop = 1; }

}  // namespace

Launcher::Launcher(transport::TransportMode mode) : mode_(mode) {}

bool Launcher::start(const DagParseResult& dag, std::string* error) {
  node_ = std::make_unique<Node>(mode_);
  for (const auto& cfg : dag.components) {
    auto comp = ComponentFactory::instance().create(cfg.type, cfg.name);
    if (comp == nullptr) {
      if (error != nullptr) {
        *error = "unknown component type '" + cfg.type + "'";
      }
      stop();
      return false;
    }
    if (!comp->init()) {
      if (error != nullptr) {
        *error = "init failed for '" + cfg.name + "'";
      }
      stop();
      return false;
    }
    if (!comp->launch(*node_, cfg.input_channels, cfg.interval)) {
      if (error != nullptr) {
        *error = "launch failed for '" + cfg.name + "'";
      }
      stop();
      return false;
    }
    started_.push_back(comp.get());
    components_.push_back(std::move(comp));
  }
  return true;
}

void Launcher::run_until_signal() {
  static_cast<void>(std::signal(SIGINT, launch_signal_handler));   // NOLINT(misc-include-cleaner)
  static_cast<void>(std::signal(SIGTERM, launch_signal_handler));  // NOLINT(misc-include-cleaner)
  while (g_launch_stop == 0) {
    pause();
  }
  stop();
}

void Launcher::stop() {
  // Quiesce ALL self-driven threads before touching any component state:
  // an in-flight timer publish can synchronously dispatch into another
  // component's Writer/CacheBuffer, so every thread must be parked before
  // the first component (or its members) is shut down or destroyed.
  for (auto* comp : std::ranges::reverse_view(started_)) {
    comp->quiesce();
  }
  for (auto* comp : std::ranges::reverse_view(started_)) {
    comp->shutdown();
  }
  started_.clear();
  components_.clear();
  node_.reset();
}

}  // namespace tianshu::core
