#ifdef __linux__

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>

#include "library/DirEvents.h"

namespace mira::library {
namespace {

constexpr std::uint32_t kEntryMask = IN_CREATE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE;
// Editors save by writing a new file and renaming it over, others by writing in place.
constexpr std::uint32_t kWriteMask = IN_CLOSE_WRITE | IN_MOVED_TO;

void Signal(int fd) {
  const std::uint64_t one = 1;
  [[maybe_unused]] auto _ = ::write(fd, &one, sizeof(one));
}

void Drain(int fd) {
  std::uint64_t count = 0;
  [[maybe_unused]] auto _ = ::read(fd, &count, sizeof(count));
}

}  // namespace

// All made here, before Run() starts on another thread: Stop() from the owner's thread must never
// race their setup. TSan caught that race (tests/watcher_test.cpp).
DirEvents::DirEvents() {
  inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
  timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  stop_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  reload_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (!Ok()) return;
  for (const int fd : {inotify_fd_, timer_fd_, stop_fd_, reload_fd_}) {
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
  }
}

DirEvents::~DirEvents() {
  for (const int fd : {inotify_fd_, epoll_fd_, timer_fd_, stop_fd_, reload_fd_}) {
    if (fd >= 0) ::close(fd);
  }
}

bool DirEvents::Ok() const {
  return inotify_fd_ >= 0 && epoll_fd_ >= 0 && timer_fd_ >= 0 && stop_fd_ >= 0 && reload_fd_ >= 0;
}

int DirEvents::Add(const std::filesystem::path& dir, bool writes) {
  return inotify_add_watch(inotify_fd_, dir.c_str(), writes ? kWriteMask : kEntryMask);
}

void DirEvents::Remove(int wd) {
  inotify_rm_watch(inotify_fd_, wd);
}

void DirEvents::ArmTimer(std::optional<int> ms) {
  itimerspec spec{};  // all-zero disarms it
  if (ms) {
    spec.it_value.tv_sec = *ms / 1000;
    spec.it_value.tv_nsec = (*ms % 1000) * 1000000L;
  }
  timerfd_settime(timer_fd_, 0, &spec, nullptr);
}

void DirEvents::WakeStop() {
  if (stop_fd_ >= 0) Signal(stop_fd_);
}

void DirEvents::WakeReload() {
  if (reload_fd_ >= 0) Signal(reload_fd_);
}

bool DirEvents::Wait(Fired& fired) {
  fired = {};
  epoll_event events[8];
  int n;
  do {
    n = epoll_wait(epoll_fd_, events, 8, -1);  // blocks; no timeout at rest
  } while (n < 0 && errno == EINTR);
  if (n < 0) return false;
  for (int i = 0; i < n; ++i) {
    const int fd = events[i].data.fd;
    if (fd == stop_fd_) {
      fired.stop = true;
    } else if (fd == reload_fd_) {
      Drain(reload_fd_);
      fired.reload = true;
    } else if (fd == timer_fd_) {
      Drain(timer_fd_);
      fired.timer = true;
    } else if (fd == inotify_fd_) {
      ReadInotify(fired.changes);
    }
  }
  return true;
}

void DirEvents::ReadInotify(std::vector<Change>& changes) {
  // inotify_event is variable-length (the name follows it), so several arrive per read.
  alignas(inotify_event) char buffer[4096];
  while (true) {
    const ssize_t n = ::read(inotify_fd_, buffer, sizeof(buffer));
    if (n <= 0) return;  // EAGAIN (nothing more queued) or an error either way
    for (size_t offset = 0; offset < static_cast<size_t>(n);) {
      const auto* event = reinterpret_cast<const inotify_event*>(buffer + offset);
      offset += sizeof(inotify_event) + event->len;
      if (event->mask & IN_Q_OVERFLOW) {
        changes.push_back({.wd = -1, .name = {}, .kind = Kind::kOverflow});
      } else if (event->mask & IN_IGNORED) {
        changes.push_back({.wd = event->wd, .name = {}, .kind = Kind::kGone});
      } else if (event->len > 0) {
        Kind kind;
        if (event->mask & (IN_CREATE | IN_MOVED_TO)) {
          kind = Kind::kAdded;
        } else if (event->mask & IN_DELETE) {
          kind = Kind::kRemoved;
        } else if (event->mask & IN_MOVED_FROM) {
          kind = Kind::kMovedAway;
        } else if (event->mask & IN_CLOSE_WRITE) {
          kind = Kind::kWritten;
        } else {
          continue;
        }
        changes.push_back({.wd = event->wd, .name = event->name, .kind = kind});
      }
    }
  }
}

}  // namespace mira::library

#endif
