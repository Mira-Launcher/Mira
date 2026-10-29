#pragma once

#include <cstdint>
#include <json.hpp>
#include <optional>
#include <string>

#include "config/Config.h"
#include "core/Result.h"

namespace mira::library {

struct InstallSize {
  std::int64_t download_bytes = -1;  // -1: the store doesn't say
  std::int64_t install_bytes = -1;
};

// What installing a store title downloads and takes on disk, as its store tool reports it: Epic
// through `legendary info`, GOG through `gogdl info`, Amazon through `nile install --info`. Kept
// for the session, so an install's progress knows its total; other stores answer
// `size_unsupported`.
Result<InstallSize> FetchInstallSize(const config::Config& config, const std::string& source,
                                     const std::string& ref);

// A size FetchInstallSize found earlier this session, without asking the store again.
std::optional<InstallSize> KnownInstallSize(const std::string& source, const std::string& ref);

// A guess at what a game takes on disk from the storage line of Steam's system requirements in its
// metadata ("Storage: 15 GB available space", "Hard Disk Space: 200MB"), the larger of minimum and
// recommended; -1 without one.
std::int64_t EstimatedInstallBytes(const nlohmann::json& metadata);

// The sizes in a store tool's info output: the first object with `download_size` or
// `disk_size`, preferring GOG's all-languages entry ("*").
std::optional<InstallSize> ParseInstallSize(std::string_view output);

}  // namespace mira::library
