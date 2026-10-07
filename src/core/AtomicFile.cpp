#include "core/AtomicFile.h"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <format>

namespace mira {

Result<void> WriteFileAtomic(const std::filesystem::path& path, std::string_view content, std::string code,
                             bool durable) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);

  // Unique per writer, so concurrent saves of one file never share a temp.
  static std::atomic<unsigned> next{0};
  const std::string temp = std::format("{}.tmp{}-{}", path.string(), getpid(), next++);
  const auto failed = [&](const std::string& message) {
    std::filesystem::remove(temp, ec);
    return Err(code, message, kDiskHint);
  };

  const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return Err(code, std::format("couldn't write {}: {}", temp, std::strerror(errno)), kDiskHint);
  std::size_t written = 0;
  while (written < content.size()) {
    const ssize_t n = ::write(fd, content.data() + written, content.size() - written);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      const std::string message = std::strerror(errno);
      ::close(fd);
      return failed(std::format("couldn't write {}: {}", temp, message));
    }
    written += static_cast<std::size_t>(n);
  }
  if (durable) ::fsync(fd);
  if (::close(fd) != 0) return failed(std::format("couldn't write {}: {}", temp, std::strerror(errno)));

  std::filesystem::rename(temp, path, ec);
  if (ec) return failed(std::format("couldn't save {}: {}", path.string(), ec.message()));
  return {};
}

Result<std::filesystem::path> SetAside(const std::filesystem::path& path) {
  std::error_code ec;
  for (int n = 1; n < 1000; ++n) {
    const std::filesystem::path target =
        n == 1 ? path.string() + ".bad" : std::format("{}.bad.{}", path.string(), n);
    // link() fails on an existing target, unlike rename(), so a backup that appears meanwhile
    // survives.
    if (::link(path.c_str(), target.c_str()) == 0) {
      std::filesystem::remove(path, ec);
    } else if (errno == EEXIST || std::filesystem::exists(target, ec)) {
      continue;
    } else {
      std::filesystem::rename(path, target, ec);  // a filesystem without hard links
    }
    if (ec)
      return Err("set_aside_failed",
                 std::format("couldn't keep {}: {}", path.string(), ec.message()), kDiskHint);
    return target;
  }
  return Err("set_aside_failed",
             std::format("couldn't keep {}: too many earlier copies", path.string()), kDiskHint);
}

std::unexpected<Error> KeptFileError(const std::filesystem::path& path) {
  return Err("file_kept",
             std::format("{} couldn't be read, so it isn't overwritten", path.string()),
             "Fix or move the file, then restart mirad.");
}

}  // namespace mira
