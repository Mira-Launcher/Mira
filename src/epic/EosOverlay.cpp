#include "epic/EosOverlay.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <json.hpp>
#include <mutex>
#include <vector>

#include "core/AtomicFile.h"
#include "core/Files.h"
#include "core/Strings.h"
#include "epic/Legendary.h"

namespace mira::epic {
namespace {
namespace fs = std::filesystem;
using nlohmann::json;

// What legendary itself looks for to call a folder an overlay install.
constexpr std::string_view kOverlayDll = "EOSOVH-Win64-Shipping.dll";
// HKCU\Software\Epic Games\EOS, as user.reg writes the key: backslashes doubled.
constexpr std::string_view kKey = R"(software\\epic games\\eos)";
constexpr std::string_view kValue = R"("OverlayPath"=)";

bool IsOverlay(const fs::path& dir) {
  std::error_code ec;
  return fs::is_regular_file(dir / kOverlayDll, ec);
}

// Legendary's record of the overlay it installed: {install_path, version, ...}.
fs::path RecordFile() { return LegendaryConfigDir() / "overlay_install.json"; }

json Record() {
  std::ifstream file(RecordFile());
  const json record = json::parse(file, nullptr, false);
  return record.is_object() ? record : json::object();
}

// "Z:\home\..." with each backslash and quote escaped, as a user.reg string holds it.
std::string RegString(std::string_view text) {
  std::string escaped;
  for (const char ch : text) {
    if (ch == '\\' || ch == '"') escaped.push_back('\\');
    escaped.push_back(ch);
  }
  return escaped;
}

std::vector<std::string> Lines(std::string_view text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    if (end == std::string_view::npos) {
      lines.emplace_back(text.substr(start));
      break;
    }
    lines.emplace_back(text.substr(start, end - start));
    start = end + 1;
  }
  return lines;
}

// Whether `line` opens the overlay's key: "[Software\\Epic Games\\EOS] 1729..." in any case.
bool OpensKey(const std::string& line) {
  if (!line.starts_with('[')) return false;
  const std::size_t close = line.find(']');
  return close != std::string::npos && strings::ToLower(line.substr(1, close - 1)) == kKey;
}

}  // namespace

fs::path ManagedOverlayDir(const config::Config& config) {
  return config.File().parent_path() / "tools" / "eos-overlay";
}

runner::ToolStatus DetectOverlay(const config::Config& config) {
  runner::ToolStatus status;
  const fs::path dir = ManagedOverlayDir(config);
  if (!IsOverlay(dir)) return status;
  status.installed = true;
  status.source = "managed";
  status.path = (dir / kOverlayDll).string();
  const json record = Record();
  if (record.value("install_path", std::string()) == dir.string()) {
    // "1.3.7++EOSSDK+Release-Overlay-CL-57923510-Windows"
    const std::string version = record.value("version", std::string());
    status.version = version.substr(0, version.find("++"));
  }
  return status;
}

Result<void> InstallOverlay(const config::Config& config) {
  static std::mutex mutex;
  const std::lock_guard lock(mutex);
  const fs::path dir = ManagedOverlayDir(config);
  // Legendary skips the download while its record names the newest version, even once the
  // folder it names is gone (removed with the store, or deleted by hand).
  if (!IsOverlay(dir)) {
    std::error_code ec;
    fs::remove(RecordFile(), ec);
  }
  auto output = RunLegendary(config, {"-y", "eos-overlay", "install", "--path", dir.string()});
  if (!output) return std::unexpected(output.error());
  // Legendary exits 0 when the download fails, saying so only in its output.
  if (!IsOverlay(dir)) {
    const auto lines = Lines(*output);
    const auto error = std::ranges::find_if(lines.rbegin(), lines.rend(), [](const auto& line) {
      return line.find("ERROR") != std::string::npos;
    });
    return Err("overlay_failed", error != lines.rend()
                                     ? strings::Trim(*error)
                                     : std::string("legendary didn't download the EOS overlay"));
  }
  return {};
}

std::string WithOverlayPath(std::string_view user_reg, std::string_view windows_path) {
  std::vector<std::string> lines = Lines(user_reg);
  const std::string entry = std::format("{}\"{}\"", kValue, RegString(windows_path));
  const auto key = std::ranges::find_if(lines, OpensKey);
  if (key == lines.end()) {
    if (windows_path.empty()) return std::string(user_reg);
    if (!lines.empty() && !lines.back().empty()) lines.emplace_back();
    lines.emplace_back(R"([Software\\Epic Games\\EOS])");
    lines.push_back(entry);
  } else {
    const auto end = std::find_if(std::next(key), lines.end(),
                                  [](const std::string& line) { return line.starts_with('['); });
    const auto value = std::find_if(std::next(key), end, [](const std::string& line) {
      return line.starts_with(kValue);
    });
    if (windows_path.empty()) {
      if (value != end) lines.erase(value);
    } else if (value != end) {
      *value = entry;
    } else {
      // After the "#time=" line Wine writes under a key's name, if there is one.
      const auto after = std::next(key) != end && std::next(key)->starts_with('#')
                             ? std::next(key, 2)
                             : std::next(key);
      lines.insert(after, entry);
    }
  }
  std::string text;
  for (const std::string& line : lines) text += line + "\n";
  return text;
}

Result<void> SetOverlayIn(const fs::path& prefix, const fs::path& overlay) {
  const fs::path file = prefix / "user.reg";
  const auto current = files::ReadFile(file);
  if (!current) return {};  // Wine writes it when it first starts
  std::string windows_path;
  if (!overlay.empty()) {
    windows_path = "Z:" + overlay.string();
    std::ranges::replace(windows_path, '/', '\\');
  }
  const std::string changed = WithOverlayPath(*current, windows_path);
  if (changed == *current) return {};
  // A prefix whose Wine is running writes its registry back when that stops, without this; the
  // next launch sets it again.
  return WriteFileAtomic(file, changed, "overlay_failed");
}

}  // namespace mira::epic
