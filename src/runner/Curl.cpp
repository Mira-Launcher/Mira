#include "runner/Curl.h"

#include <format>

#include "runner/Exec.h"

namespace mira::runner {

Result<void> CurlDownload(const std::string& url, const std::filesystem::path& dest) {
  Command command;
  // -f: an HTTP error must fail here, not get saved as the file.
  command.argv = {"curl", "-sSLf", "--connect-timeout", "10", "--speed-limit", "1024", "--speed-time", "60",
                  "-o",   dest.string(), url};
  const Result<ExecResult> result = RunAndWait(command);
  if (result && result->exit_code == 0) return {};
  std::error_code ec;
  std::filesystem::remove(dest, ec);
  return Err("download_failed",
             std::format("couldn't download {}: {}", url,
                         !result ? result.error().message
                                 : std::format("curl exited {}: {}", result->exit_code, result->output)),
             kConnectionHint);
}

Result<nlohmann::json> CurlJson(const std::string& url, int max_time_s) {
  Command command;
  command.argv = {"curl", "-sSL", "--connect-timeout", "10", "--max-time", std::to_string(max_time_s), url};
  const Result<ExecResult> result = RunAndWait(command);
  if (!result) return std::unexpected(result.error());
  nlohmann::json parsed = nlohmann::json::parse(result->output, nullptr, false);
  if (parsed.is_discarded()) {
    return Err("fetch_failed", std::format("{} didn't answer with JSON: {}", url, result->output), kConnectionHint);
  }
  return parsed;
}

}  // namespace mira::runner
