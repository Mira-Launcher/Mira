#pragma once

#include <filesystem>
#include <string>

#include <json.hpp>

#include "core/Result.h"

namespace mira::runner {

// Downloads `url` to `dest`, removing it on failure. Only a stalled transfer ends early (under 1 KB/s for a
// minute), so a large file takes as long as it needs.
Result<void> CurlDownload(const std::string& url, const std::filesystem::path& dest);

// Fetches `url` and parses it as JSON, giving up after `max_time_s`. Err("fetch_failed") with what came
// back when it isn't JSON.
Result<nlohmann::json> CurlJson(const std::string& url, int max_time_s = 30);

}  // namespace mira::runner
