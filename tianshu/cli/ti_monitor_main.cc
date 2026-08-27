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

// ti-monitor: live channel monitor TUI (cyber_monitor-equivalent).
//
//   ti monitor <channel>... [--depth N] [--once]
//
// Keys (vi-flavoured, count prefix supported, e.g. `5j`):
//   j / k        select channel (down / up)
//   SPACE or p   pause / resume (pause locks the buffers for browsing)
//   h / l        previous / next frame        (paused only)
//   CTRL-D / CTRL-U  jump 16 frames back / forward (paused only)
//   g / G        first / last frame           (paused only)
//   q            quit
//
// --once: headless mode for CI — wait for the first frame on every
// channel, print one summary line, exit (0 ok / 1 timeout).

#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/ioctl.h>

#include "tianshu/core/monitor.h"

namespace {

constexpr std::size_t kJumpFrames = 16;

struct TermGuard {
  termios saved{};
  bool active{false};

  void enter_raw() {
    if (tcgetattr(STDIN_FILENO, &saved) != 0) {
      return;
    }
    termios raw = saved;
    raw.c_lflag &= ~(static_cast<unsigned>(ICANON) | static_cast<unsigned>(ECHO));
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) {
      active = true;
    }
  }

  static void enter_tui() {
    static_cast<void>(std::fputs("\x1b[?1049h\x1b[?25l", stdout));
    static_cast<void>(fflush(stdout));
  }

  void leave() {
    if (active) {
      static_cast<void>(tcsetattr(STDIN_FILENO, TCSANOW, &saved));
    }
    static_cast<void>(std::fputs("\x1b[?25h\x1b[?1049l", stdout));
    static_cast<void>(fflush(stdout));
  }
};

std::size_t terminal_rows() {
  winsize ws{};
  // NOLINTNEXTLINE(misc-include-cleaner)  // glibc: TIOCGWINSZ in sys/ioctl.h via bits
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
    return ws.ws_row;
  }
  return 24;
}

std::size_t terminal_cols() {
  winsize ws{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
    return ws.ws_col;
  }
  return 80;
}

void draw(const tianshu::core::MonitorUiSnapshot& snap) {
  const std::size_t rows = terminal_rows();
  const std::size_t cols = terminal_cols();
  std::string out;
  out.reserve(8192U);
  out += "\x1b[H\x1b[2J";

  const std::size_t list_width = 34;
  const std::size_t detail_width = cols > list_width + 2 ? cols - list_width - 2 : 20;

  // Channel list (rows 0..rows-3)
  for (std::size_t i = 0; i + 2 < rows && i < snap.channels.size(); ++i) {
    const auto& ch = snap.channels[i];
    const bool selected = i == snap.selected;
    char line[128];
    static_cast<void>(std::snprintf(line, sizeof(line), "%c %-22s %5.1fHz %6zuB",  // NOLINT
                                    selected ? '>' : ' ', ch.name.c_str(), ch.stats.hz,
                                    ch.stats.last_size));
    out += selected ? "\x1b[7m" : "";
    out.append(line, std::min<std::size_t>(detail_width > 0 ? list_width : 0, sizeof(line)));
    out += selected ? "\x1b[0m" : "";
    out += "\x1b[K\r\n";
  }
  for (std::size_t i = snap.channels.size(); i + 2 < rows; ++i) {
    out += "\x1b[K\r\n";
  }

  // Separator
  out += "\x1b[K\r\n";

  // Detail: frame header + hex dump of the current view frame
  if (!snap.channels.empty()) {
    const auto& ch = snap.channels[snap.selected];
    char head[160];
    static_cast<void>(std::snprintf(  // NOLINT
        head, sizeof(head), " %s  seq=%llu  ts=%lldns  size=%zu  frame %zu/%zu %s", ch.name.c_str(),
        static_cast<unsigned long long>(snap.frame.seq),
        static_cast<long long>(snap.frame.timestamp_ns), snap.frame.payload.size(),
        ch.buffered == 0 ? 0 : ch.cursor + 1, ch.buffered,
        snap.app_paused ? "[PAUSED]" : "[LIVE]"));
    out += head;
    out += "\x1b[K\r\n";

    constexpr std::size_t kBytesPerRow = 8;
    for (std::size_t row = 0; row + 3 < rows; ++row) {
      const std::size_t base = row * kBytesPerRow;
      if (base >= snap.frame.payload.size()) {
        break;
      }
      char hexline[160];
      auto written = static_cast<std::size_t>(
          std::snprintf(hexline, sizeof(hexline), " %04zx:", base));  // NOLINT
      // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      for (std::size_t b = 0; b < kBytesPerRow && base + b < snap.frame.payload.size(); ++b) {
        written += static_cast<std::size_t>(std::snprintf(  // NOLINT
            hexline + written, sizeof(hexline) - written, " %02x", snap.frame.payload[base + b]));
      }
      // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      out.append(hexline, written);
      out += "\x1b[K\r\n";
    }
  } else {
    out += " no channels\x1b[K\r\n";
  }

  // Status bar
  out += "\x1b[7m ";
  out += snap.app_paused ? "PAUSED" : "LIVE";
  out += " | j/k ch  sp pause  h/l frame  C-d/u jump  g/G ends  q quit \x1b[0m";

  static_cast<void>(std::fputs(out.c_str(), stdout));
  static_cast<void>(fflush(stdout));
}

// Returns the pending vi count (0 = none) and consumes it.
int apply_count(int& pending_count) {
  const int n = pending_count > 0 ? pending_count : 1;
  pending_count = 0;
  return n;
}

int run_tui(tianshu::core::MonitorApp& app) {
  TermGuard term;
  term.enter_raw();
  TermGuard::enter_tui();

  int pending_count = 0;
  bool quit = false;
  while (!quit) {
    draw(app.snapshot());

    // NOLINTNEXTLINE(misc-include-cleaner)  // glibc: poll.h
    pollfd pfd{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
    if (poll(&pfd, 1, 100) > 0 && (pfd.revents & POLLIN) != 0) {  // NOLINT(misc-include-cleaner)
      char buf[16];
      const ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
      for (ssize_t i = 0; i < n; ++i) {
        const char c = buf[i];
        if (c >= '0' && c <= '9') {
          pending_count = std::min((pending_count * 10) + (c - '0'), 999);
          continue;
        }
        switch (c) {
          case 'q':
            quit = true;
            break;
          case 'j':
            app.select_delta(apply_count(pending_count));
            break;
          case 'k':
            app.select_delta(-apply_count(pending_count));
            break;
          case ' ':
          case 'p':
            if (app.paused()) {
              app.resume();
            } else {
              app.pause();
            }
            break;
          case 'h':
            app.step_frame(-apply_count(pending_count));
            break;
          case 'l':
            app.step_frame(apply_count(pending_count));
            break;
          case 0x04:  // CTRL-D
            app.jump_frame_by(-static_cast<std::int64_t>(kJumpFrames) * apply_count(pending_count));
            break;
          case 0x15:  // CTRL-U
            app.jump_frame_by(static_cast<std::int64_t>(kJumpFrames) * apply_count(pending_count));
            break;
          case 'g':
            app.jump_frame_first();
            pending_count = 0;
            break;
          case 'G':
            app.jump_frame_last();
            pending_count = 0;
            break;
          default:
            pending_count = 0;
            break;
        }
      }
    }
  }

  term.leave();
  return 0;
}

int run_once(tianshu::core::MonitorApp& app) {
  const std::size_t missing = app.wait_first_frames(std::chrono::milliseconds(3000));
  const auto snap = app.snapshot();
  for (const auto& ch : snap.channels) {
    static_cast<void>(std::printf(  // NOLINT(concurrency-mt-unsafe)
        "%s: hz=%.1f seq=%llu size=%zu buffered=%zu\n", ch.name.c_str(), ch.stats.hz,
        static_cast<unsigned long long>(ch.stats.last_seq), ch.stats.last_size, ch.buffered));
  }
  return missing == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> channels;
  std::size_t depth = 512;
  bool once = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (arg == "--depth" && i + 1 < argc) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      depth = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (arg == "--once") {
      once = true;
    } else if (!arg.starts_with("--")) {
      channels.push_back(arg);
    } else {
      static_cast<void>(std::fprintf(stderr, "ti-monitor: unknown option %s\n", arg.c_str()));
      return 2;
    }
  }
  if (channels.empty()) {
    static_cast<void>(
        std::fprintf(stderr, "usage: ti-monitor <channel>... [--depth N] [--once]\n"));
    return 2;
  }

  tianshu::core::MonitorApp app(depth);
  for (const auto& ch : channels) {
    if (!app.add_channel(ch)) {
      static_cast<void>(std::fprintf(stderr, "ti-monitor: cannot attach %s\n", ch.c_str()));
      return 1;
    }
  }

  if (once) {
    return run_once(app);
  }

  return run_tui(app);
}
