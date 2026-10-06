#pragma once

#include <filesystem>
#include <string>

#include <json.hpp>

#include "core/Result.h"
#include "runner/Exec.h"

namespace mira::runner {

// The curl command CurlDownload runs: it retries through a dropped connection or a timeout (every kind of
// error, for up to ten minutes) instead of failing on the first, and only a transfer that stalls (under 1 KB/s for
// a minute) ends early.
Command CurlDownloadCommand(const std::string& url, const std::filesystem::path& dest);

// Downloads `url` to `dest`, removing it on failure. A large file takes as long as it needs, and a network that
// drops and comes back is waited out.
Result<void> CurlDownload(const std::string& url, const std::filesystem::path& dest);

// Fetches `url` and parses it as JSON, giving up after `max_time_s`. Err("fetch_failed") with what came
// back when it isn't JSON.
Result<nlohmann::json> CurlJson(const std::string& url, int max_time_s = 30);

}  // namespace mira::runner
