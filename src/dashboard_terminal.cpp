// Port of dashboard/renderer.py (nevermind455/oct10@772f2eb): one frame at a
// time, only the rows that changed, on a thread of its own. Nothing here
// touches the bot, and a slow or closed terminal cannot hold it up.
#include "sniper/dashboard.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace sniper::dash {
namespace {
const std::string ALT_ON = "\x1b[?1049h", ALT_OFF = "\x1b[?1049l",
                  CURSOR_OFF = "\x1b[?25l", CURSOR_ON = "\x1b[?25h",
                  WRAP_OFF = "\x1b[?7l", WRAP_ON = "\x1b[?7h",
                  CLEAR = "\x1b[2J\x1b[H", ERASE_LINE = "\x1b[K",
                  // Synchronized output: terminals that support it show the
                  // whole frame at once; the rest ignore it.
                  SYNC_ON = "\x1b[?2026h", SYNC_OFF = "\x1b[?2026l",
                  RESET = "\x1b[0m";

ColorMode color_mode() {
  auto env = [](const char *name) {
    const char *v = std::getenv(name);
    return std::string(v ? v : "");
  };
  auto forced = env("TERM_COLOR_MODE");
  if (forced == "truecolor")
    return ColorMode::truecolor;
  if (forced == "xterm256")
    return ColorMode::xterm256;
  if (forced == "mono" || !env("NO_COLOR").empty())
    return ColorMode::mono;
  auto colorterm = env("COLORTERM");
  if (colorterm == "truecolor" || colorterm == "24bit")
    return ColorMode::truecolor;
  auto term = env("TERM");
  if (term.find("256") != std::string::npos ||
      term.find("truecolor") != std::string::npos)
    return ColorMode::truecolor;
  return !term.empty() && term != "dumb" ? ColorMode::xterm256 : ColorMode::mono;
}
} // namespace

struct Dashboard::State {
  int fd = -1;
  bool interactive = false;
  std::mutex mutex;
  std::condition_variable wake;
  bool stop = false, has_snapshot = false, finished = false;
  Json pending;

  ColorMode mode = ColorMode::mono;
  std::vector<std::string> prev;
  int prev_cols = 0, prev_rows = 0;
  bool force = true, dead = false;
  std::chrono::steady_clock::time_point last_plain{};

  bool stopping() {
    std::lock_guard lock(mutex);
    return stop;
  }
  // Every byte, or false once the terminal is gone. Each wait is short, so
  // shutdown is not held up by a terminal that stopped reading.
  bool write_all(std::string_view data) {
    while (!data.empty()) {
      pollfd p{fd, POLLOUT, 0};
      int ready = ::poll(&p, 1, 200);
      if (ready < 0 && errno == EINTR)
        continue;
      if (ready < 0)
        return false;
      if (ready == 0) {
        if (stopping())
          return false;
        continue;
      }
      if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
        return false;
      auto n = ::write(fd, data.data(), std::min<std::size_t>(data.size(), 4096));
      if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
        continue;
      if (n <= 0)
        return false;
      data.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
  }
  std::pair<int, int> size() const {
    winsize w{};
    int cols = 120, rows = 40;
    if (::ioctl(fd, TIOCGWINSZ, &w) == 0 && w.ws_col > 0 && w.ws_row > 0) {
      cols = w.ws_col;
      rows = w.ws_row;
    }
    return {std::max(40, cols), std::max(10, rows)};
  }
  void start_screen() {
    mode = color_mode();
    if (!write_all(ALT_ON + CURSOR_OFF + WRAP_OFF + CLEAR))
      dead = true;
  }
  void end_screen() {
    if (!dead)
      write_all(RESET + WRAP_ON + CURSOR_ON + ALT_OFF);
  }
  void draw(const Json &snap) {
    auto [cols, rows] = size();
    if (cols != prev_cols || rows != prev_rows) {
      prev_cols = cols;
      prev_rows = rows;
      force = true;
    }
    auto frame = build(snap, cols, rows);
    std::vector<std::string> lines;
    lines.reserve(frame.size());
    for (auto &row : frame)
      lines.push_back(render_row(row, mode));
    std::string buf;
    for (std::size_t i = 0; i < lines.size(); ++i)
      if (force || i >= prev.size() || prev[i] != lines[i])
        buf += "\x1b[" + std::to_string(i + 1) + ";1H" + lines[i];
    if (force)
      // Never a full clear here: that blanks the screen for a frame. Only rows
      // past the end of a shorter frame need erasing.
      for (std::size_t i = lines.size(); i < prev.size(); ++i)
        buf += "\x1b[" + std::to_string(i + 1) + ";1H" + ERASE_LINE;
    if (!buf.empty()) {
      buf += "\x1b[" + std::to_string(lines.size()) + ";1H";
      if (!write_all(SYNC_ON + buf + SYNC_OFF)) {
        dead = true;
        return;
      }
    }
    force = false;
    prev = std::move(lines);
  }
  // Not a terminal (a log file, systemd): one plain line every 10 seconds, with
  // no escape codes.
  void plain(const Json &snap) {
    auto now = std::chrono::steady_clock::now();
    if (last_plain.time_since_epoch().count() != 0 &&
        now - last_plain < std::chrono::seconds(10))
      return;
    last_plain = now;
    if (!write_all(plain_line(snap) + "\n"))
      dead = true;
  }
  void run() {
    if (interactive)
      start_screen();
    for (;;) {
      Json snap;
      {
        std::unique_lock lock(mutex);
        wake.wait(lock, [this] { return stop || has_snapshot; });
        if (stop)
          break;
        snap = std::move(pending);
        pending = Json();
        has_snapshot = false;
      }
      if (dead)
        continue;
      try {
        if (interactive)
          draw(snap);
        else
          plain(snap);
      } catch (...) {
        // A display fault must never reach the bot; the next frame tries again.
        force = true;
      }
    }
    if (interactive)
      end_screen();
    std::lock_guard lock(mutex);
    finished = true;
    wake.notify_all();
  }
};

Dashboard::Dashboard(int fd) : s_(std::make_shared<State>()) {
  s_->fd = fd;
  s_->interactive = ::isatty(fd) == 1;
  // The thread keeps its own reference: if a stuck terminal outlasts stop(),
  // it is left behind safely rather than holding up the exit.
  thread_ = std::thread([state = s_] { state->run(); });
}

Dashboard::~Dashboard() { stop(); }

bool Dashboard::interactive() const { return s_->interactive; }

void Dashboard::submit(Json snapshot) {
  {
    std::lock_guard lock(s_->mutex);
    if (s_->stop)
      return;
    s_->pending = std::move(snapshot);
    s_->has_snapshot = true;
  }
  s_->wake.notify_all();
}

void Dashboard::stop() {
  if (!thread_.joinable())
    return;
  bool finished;
  {
    std::unique_lock lock(s_->mutex);
    s_->stop = true;
    s_->wake.notify_all();
    finished = s_->wake.wait_for(lock, std::chrono::seconds(2),
                                 [this] { return s_->finished; });
  }
  if (finished)
    thread_.join();
  else
    thread_.detach();
}
} // namespace sniper::dash
