#include "humble/Humble.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <ranges>
#include <regex>

#include "config/RunnerSources.h"
#include "core/AtomicFile.h"
#include "core/Files.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/StoreErrors.h"
#include "core/Strings.h"
#include "runner/Exec.h"

namespace mira::humble {
namespace {

std::filesystem::path KeyFile() {
  return paths::Home() / ".humble-cli-key";
}
namespace fs = std::filesystem;

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
  return config.File().parent_path() / "tools" / "humble" /
         std::string(config::runner_sources::kHumbleCliBinaryName);
}

const runner::StoreTool kTool = {"humble", "Humble Bundle", "humble-cli", "humble.humble_cli_bin",
                                 ManagedHumbleCliPath};

runner::ToolStatus DetectHumbleCli(const config::Config& config) {
  return runner::DetectTool(config, kTool);
}

Result<void> InstallHumbleCliBinary(const config::Config& config,
                                    const runner::ReleaseAsset& asset) {
  auto installed = runner::InstallToolBinary(
      config, "humble", asset, std::string(config::runner_sources::kHumbleCliBinaryName));
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

  // No "am I logged in" call exists, so `list` stands in. Only its no-key and rejected-key
  // messages mean signed out; a network error or crash doesn't.
  const Result<std::string> listed = RunHumbleCli(config, {"list", "--field", "key"});
  status.authenticated = listed || !(listed.error().message.contains("config file not found") ||
                                     listed.error().message.contains("config file is empty") ||
                                     listed.error().message.contains("Unauthorized"));
  return status;
}

std::optional<std::string> FindSessionKey(std::string_view text) {
  static const std::regex cookie(R"re(^\s*("?eyJ[A-Za-z0-9+/_=-]+\|\d+\|[0-9a-fA-F]+"?)\s*$)re");
  std::match_results<std::string_view::const_iterator> match;
  if (!std::regex_match(text.begin(), text.end(), match, cookie)) return std::nullopt;
  return match[1].str();
}

Result<void> Login(const config::Config& config, const std::string& session_key) {
  // What `humble-cli auth <key>` writes, without the key in a process's arguments for anyone to
  // read.
  const std::filesystem::path key_file = KeyFile();
  const std::optional<std::string> previous = files::ReadFile(key_file);
  if (auto written =
          WriteFileAtomic(key_file, strings::Trim(session_key), "login_failed", true, 0600);
      !written) {
    return written;
  }
  if (const runner::AuthStatus status = Status(config); !status.authenticated) {
    // A rejected key doesn't log out a working one.
    std::error_code ec;
    if (previous) {
      if (auto restored = WriteFileAtomic(key_file, *previous, "login_failed", true, 0600);
          !restored) {
        log::Error("couldn't put the previous Humble key back: {}", restored.error().message);
      }
    } else {
      std::filesystem::remove(key_file, ec);
    }
    return Err("login_failed", "humble-cli didn't accept that session key");
  }
  return {};
}

Result<void> Logout(const config::Config&) {
  std::error_code ec;
  std::filesystem::remove(KeyFile(), ec);
  if (ec)
    return Err("logout_failed",
               std::format("couldn't remove {}: {}", KeyFile().string(), ec.message()));
  return {};
}

Result<std::vector<BundleSummary>> ListBundles(const config::Config& config) {
  const Result<std::string> output =
      RunHumbleCli(config, {"list", "--field", "key", "--field", "name", "--field", "claimed"});
  if (!output) return std::unexpected(output.error());

  std::vector<BundleSummary> bundles;
  size_t pos = 0;
  bool skipped_header = false;
  while (pos <= output->size()) {
    const size_t newline = output->find('\n', pos);
    const std::string line =
        output->substr(pos, newline == std::string::npos ? std::string::npos : newline - pos);
    if (newline == std::string::npos)
      pos = output->size() + 1;
    else
      pos = newline + 1;

    const std::string trimmed = strings::Trim(line);
    if (trimmed.empty()) continue;
    const std::vector<std::string> columns = ParseCsvLine(trimmed);
    if (columns.size() < 2) continue;

    // No header row in real output, but skip one defensively if a future
    // humble-cli version adds one -- same "first column reads exactly
    // 'key'" check either way.
    if (!skipped_header) {
      const std::string lowered = strings::ToLower(columns[0]);
      if (lowered == "key") {
        skipped_header = true;
        continue;
      }
    }

    BundleSummary bundle;
    bundle.key = columns[0];
    bundle.name = columns[1];
    const std::string claimed = strings::ToLower(columns.size() > 2 ? columns[2] : std::string());
    bundle.claimed = claimed == "yes" || claimed == "true";
    bundles.push_back(std::move(bundle));
  }
  return bundles;
}

std::filesystem::path DownloadDir(const config::Config& config, const std::string& bundle_key) {
  return config.GetPath("humble.download_root") / bundle_key;
}

Result<bool> Download(const config::Config& config, const std::string& bundle_key,
                      const std::string& item_numbers) {
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
    return Err("download_failed",
               std::format("humble-cli exited {}: {}", result->exit_code, result->output));
  }
  // A purchase that's a redeemed Steam key with no
  // Humble-hosted files (`humble-cli details` shows "No items to show",
  // "Total size: 0 B") still exits 0 here, printing this exact line
  // instead of downloading anything.
  if (result->output.find("Nothing to download") != std::string::npos) return false;
  return true;
}

}  // namespace mira::humble
