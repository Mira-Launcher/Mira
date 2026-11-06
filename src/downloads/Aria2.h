#pragma once

#include <sys/types.h>

#include <cstdint>
#include <filesystem>
#include <json.hpp>
#include <mutex>
#include <string>
#include <vector>

#include "core/Result.h"

namespace mira::downloads {

struct FileStatus {
  std::string state;  // active, waiting, paused, error, complete or removed
  std::int64_t total = 0;
  std::int64_t done = 0;
  std::int64_t speed = 0;  // bytes a second
  std::string error;       // when state is error
};

// aria2c, run on demand and talked to over its RPC port. It keeps its own control files beside each
// download, so adding the same file to the same folder later continues it.
class Aria2 {
 public:
  Aria2() = default;
  ~Aria2();
  Aria2(const Aria2&) = delete;
  Aria2& operator=(const Aria2&) = delete;

  // Adds a file, or several addresses for the same one, and returns its gid. `name` overrides the
  // name the server gives it.
  Result<std::string> Add(const std::vector<std::string>& urls, const std::filesystem::path& dir,
                          const std::string& name);
  Result<void> Pause(const std::string& gid);
  Result<void> Resume(const std::string& gid);
  // Stops it and forgets it; its files stay.
  void Remove(const std::string& gid);
  Result<FileStatus> Status(const std::string& gid);
  // 0 is no limit.
  Result<void> SetSpeedLimit(std::int64_t bytes_per_second);

 private:
  Result<void> Ensure();
  void Kill();
  Result<nlohmann::json> Call(const std::string& method, nlohmann::json params);

  std::recursive_mutex mutex_;
  pid_t pid_ = -1;
  int port_ = 0;
  std::string secret_;
  std::int64_t limit_ = 0;
};

}  // namespace mira::downloads
