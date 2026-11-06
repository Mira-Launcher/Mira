#pragma once

#include <cstdint>
#include <json.hpp>
#include <memory>
#include <string>
#include <vector>

#include "config/Config.h"
#include "core/Result.h"
#include "downloads/Links.h"

namespace mira::downloads {

// Add's answer when `only_if_cached` was asked and the service doesn't have it.
inline constexpr const char* kNotCached = "not_cached";
// The service refused the key: no point trying again until the user changes it.
inline constexpr const char* kServiceKeyRejected = "service_key_rejected";

struct ServiceFile {
  std::string id;
  std::string path;  // relative, safe to write under the download's folder
  std::int64_t size = 0;
};

struct ServiceStatus {
  bool ready = false;
  double progress = 0;  // 0 to 1
  std::string failure;  // why the service gave up on it; empty while it's fine
  std::vector<ServiceFile> files;
};

// A download service, such as TorBox. It fetches a link on its own servers, then Mira downloads the
// result from there.
class Service {
 public:
  virtual ~Service() = default;
  virtual std::string Name() const = 0;
  // Gives it a link and answers a job id. Adding a link it already has answers the same job. With
  // `only_if_cached`, Err(kNotCached) when it doesn't have the link ready, adding nothing.
  virtual Result<std::string> Add(const std::string& link, bool only_if_cached) = 0;
  virtual Result<ServiceStatus> Status(const std::string& job) = 0;
  virtual Result<std::string> FileAddress(const std::string& job, const std::string& file) = 0;
  // The files of a link it already has, without adding it. Empty when it hasn't or can't tell.
  virtual std::vector<LinkFile> Peek(const std::string& link) = 0;
};

// The service set up in Settings, or null.
std::unique_ptr<Service> ActiveService(const config::Config& config);

// What a TorBox torrent or web download entry says. Pure, so it can be tested.
ServiceStatus ParseTorBoxItem(const nlohmann::json& item);
// A remote file's path as relative parts that stay under the folder it's written to.
std::string SafeRelativePath(const std::string& path);

}  // namespace mira::downloads
