#include "humble/Humble.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <ranges>

#include "config/RunnerSources.h"
#include "core/StoreErrors.h"
#include "runner/Exec.h"

namespace mira::humble {
namespace {
namespace fs = std::filesystem;

std::string Trim(std::string text) {
  const auto not_space = [](unsigned char c) { return !std::isspace(c); };
  text.erase(text.begin(), std::ranges::find_if(text, not_space));
  text.erase(std::ranges::find_if(text | std::views::reverse, not_space).base(), text.end());
  return text;
}

// humble-cli's --field output is plain CSV, not a padded
// table (no header row either) -- "pS5kGAW5APbRTHH7,Surviving Mars -
// Deluxe Edition,Yes". Handles a quoted field (a title containing a
// comma) the standard way, matching Go's encoding/csv default dialect,
// which this is almost certainly built with.
std::vector<std::string> ParseCsvLine(const std::string& line) {
  std::vector<std::string> fields;
  std::string field;
  bool in_quotes = false;
  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (in_quotes) {
      if (c == '"') {
        if (i + 1 < line.size() && line[i + 1] == '"') {
          field += '"';
          ++i;
        } else {
          in_quotes = false;
        }
      } else {
        field += c;
      }
    } else if (c == '"') {
      in_quotes = true;
    } else if (c == ',') {
      fields.push_back(std::move(field));
      field.clear();
    } else {
      field += c;
    }
  }
  fields.push_back(std::move(field));
  return fields;
}

}  // namespace

std::filesystem::path ManagedHumbleCliPath(const config::Config& config) {
  return config.File().parent_path() / "tools" / "humble" / std::string(config::runner_sources::kHumbleCliBinaryName);
}

const runner::StoreTool kTool = {"humble", "Humble Bundle", "humble-cli", "humble.humble_cli_bin", ManagedHumbleCliPath};

runner::ToolStatus DetectHumbleCli(const config::Config& config) { return runner::DetectTool(config, kTool); }

Result<void> InstallHumbleCliBinary(const config::Config& config, const runner::ReleaseAsset& asset) {
  auto installed =
    runner::InstallToolBinary(config, "humble", asset, std::string(config::runner_sources::kHumbleCliBinaryName));
  if (!installed) return std::unexpected(installed.error());
  return {};
}

Result<std::string> RunHumbleCli(const config::Config& config, std::vector<std::string> args) {
  return runner::RunTool(config, kTool, args, {});
}

runner::AuthStatus Status(const config::Config& config) {
  runner::AuthStatus status;
  status.tool = DetectHumbleCli(config);
  if (!status.tool.installed) return status;

  // No separate "am I logged in" call exists -- `list` is the cheapest
  // real one, and fails with a specific, recognizable message
  // ("config file not found...") when no session key has been set yet.
  const Result<std::string> listed = RunHumbleCli(config, {"list", "--field", "key"});
  status.authenticated = listed.has_value();
  return status;
}

Result<void> Login(const config::Config& config, const std::string& session_key) {
  if (auto output = RunHumbleCli(config, {"auth", session_key}); !output) return std::unexpected(output.error());
  if (const runner::AuthStatus status = Status(config); !status.authenticated) {
    return Err("login_failed", "humble-cli didn't accept that session key");
  }
  return {};
}

Result<std::vector<BundleSummary>> ListBundles(const config::Config& config) {
  const Result<std::string> output = RunHumbleCli(config, {"list", "--field", "key", "--field", "name",
                                                          "--field", "claimed"});
  if (!output) return std::unexpected(output.error());

  std::vector<BundleSummary> bundles;
  size_t pos = 0;
  bool skipped_header = false;
  while (pos <= output->size()) {
    const size_t newline = output->find('\n', pos);
    const std::string line = output->substr(pos, newline == std::string::npos ? std::string::npos : newline - pos);
    if (newline == std::string::npos) pos = output->size() + 1;
    else pos = newline + 1;

    const std::string trimmed = Trim(line);
    if (trimmed.empty()) continue;
    const std::vector<std::string> columns = ParseCsvLine(trimmed);
    if (columns.size() < 2) continue;

    // No header row in real output, but skip one defensively if a future
    // humble-cli version adds one -- same "first column reads exactly
    // 'key'" check either way.
    if (!skipped_header) {
      std::string lowered = columns[0];
      std::ranges::transform(lowered, lowered.begin(), [](unsigned char c) { return std::tolower(c); });
      if (lowered == "key") {
        skipped_header = true;
        continue;
      }
    }

    BundleSummary bundle;
    bundle.key = columns[0];
    bundle.name = columns[1];
    std::string claimed = columns.size() > 2 ? columns[2] : std::string();
    std::ranges::transform(claimed, claimed.begin(), [](unsigned char c) { return std::tolower(c); });
    bundle.claimed = claimed == "yes" || claimed == "true";
    bundles.push_back(std::move(bundle));
  }
  return bundles;
}

std::filesystem::path DownloadDir(const config::Config& config, const std::string& bundle_key) {
  return config.GetPath("humble.download_root") / bundle_key;
}

Result<bool> Download(const config::Config& config, const std::string& bundle_key, const std::string& item_numbers) {
  const fs::path dir = DownloadDir(config, bundle_key);
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) return Err("download_dir_failed", ec.message());

  const runner::ToolStatus status = DetectHumbleCli(config);
  if (!status.installed) return StoreToolMissing("humble", "Humble Bundle", "humble-cli");

  Command command;
  command.argv = {status.path, "download", bundle_key, "--cur-dir"};
  if (!item_numbers.empty()) {
    command.argv.push_back("--item-numbers");
    command.argv.push_back(item_numbers);
  }
  command.cwd = dir;
  const Result<runner::ExecResult> result = runner::RunAndWait(command);
  if (!result) return std::unexpected(result.error());
  if (result->exit_code != 0) {
    return Err("download_failed", std::format("humble-cli exited {}: {}", result->exit_code, result->output));
  }
  // A purchase that's a redeemed Steam key with no
  // Humble-hosted files (`humble-cli details` shows "No items to show",
  // "Total size: 0 B") still exits 0 here, printing this exact line
  // instead of downloading anything.
  if (result->output.find("Nothing to download") != std::string::npos) return false;
  return true;
}

}  // namespace mira::humble
