#include "steam/GridArt.h"

#include <array>
#include <json.hpp>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include "core/AtomicFile.h"
#include "core/Files.h"
#include "core/Log.h"
#include "core/Strings.h"

namespace mira::steam {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

// Steam's name for each slot after the app id. The wide capsule (<appid>.png) has no slot in Mira.
constexpr std::array<std::pair<std::string_view, std::string_view>, 4> kSuffixes = {
    {{"cover", "p"}, {"hero", "_hero"}, {"logo", "_logo"}, {"icon", "_icon"}}};
constexpr std::array<std::string_view, 2> kExtensions = {".png", ".jpg"};
constexpr std::string_view kBackup = ".before-mira";

// Paths relative to userdata/, so a moved Steam folder still matches.
std::set<std::string> ReadWritten(const fs::path& file) {
  std::set<std::string> written;
  const auto text = files::ReadFile(file);
  if (!text) return written;
  const json list = json::parse(*text, nullptr, false);
  if (!list.is_array()) return written;
  for (const json& entry : list) {
    if (entry.is_string()) written.insert(entry.get<std::string>());
  }
  return written;
}

Result<void> SaveWritten(const fs::path& file, const std::set<std::string>& written) {
  return WriteFileAtomic(file, json(written).dump(2) + "\n", "steam_art_write_failed");
}

fs::path BackupOf(const fs::path& file) {
  fs::path backup = file;
  backup += kBackup;
  return backup;
}

// Puts back the user's art set aside under either extension of `base` (<grid>/<appid><suffix>).
void RestoreSlot(const fs::path& base) {
  std::error_code ec;
  for (const std::string_view extension : kExtensions) {
    fs::path file = base;
    file += extension;
    if (!fs::exists(file, ec) && fs::exists(BackupOf(file), ec))
      fs::rename(BackupOf(file), file, ec);
  }
}

// Moves the user's own art out of the way; false when an earlier copy already holds the name.
bool SetAside(const fs::path& file) {
  std::error_code ec;
  if (!fs::exists(file, ec)) return true;
  if (fs::exists(BackupOf(file), ec)) return false;
  fs::rename(file, BackupOf(file), ec);
  return !ec;
}

std::vector<fs::path> Accounts(const fs::path& steam_root) {
  std::vector<fs::path> accounts;
  std::error_code ec;
  for (const fs::directory_entry& account : fs::directory_iterator(steam_root / "userdata", ec)) {
    // "0" is Steam's placeholder before anyone signs in.
    if (account.path().filename() == "0" || !fs::is_directory(account.path() / "config", ec))
      continue;
    accounts.push_back(account.path());
  }
  return accounts;
}

std::string Extension(const fs::path& file) {
  const std::string extension = strings::ToLower(file.extension().string());
  return extension == ".jpeg" ? ".jpg" : extension;
}

}  // namespace

Result<void> SyncGridArt(const fs::path& steam_root, const std::string& appid,
                         const GridPicks& picks, const fs::path& written_file) {
  std::set<std::string> written = ReadWritten(written_file);
  std::optional<Error> failure;
  const fs::path userdata = steam_root / "userdata";
  for (const fs::path& account : Accounts(steam_root)) {
    const fs::path grid = account / "config" / "grid";
    for (const auto& [slot, suffix] : kSuffixes) {
      const std::string stem = appid + std::string(suffix);
      const auto pick = picks.find(std::string(slot));
      std::string extension = pick == picks.end() ? std::string() : Extension(pick->second);
      if (!extension.empty() && !std::ranges::contains(kExtensions, extension)) {
        log::Info("Steam can't show {}, leaving its {} as it is", pick->second.string(), slot);
        extension.clear();
      }
      // Steam shows either extension, so the user's art goes aside under both.
      bool clear = true;
      for (const std::string_view other : kExtensions) {
        const fs::path file = grid / (stem + std::string(other));
        const std::string key = file.lexically_relative(userdata).generic_string();
        std::error_code ec;
        if (written.contains(key)) {
          if (other == extension) continue;  // overwritten below
          fs::remove(file, ec);
          written.erase(key);
        } else if (!extension.empty() && !SetAside(file)) {
          clear = false;
        }
      }
      if (!clear) {
        log::Warn("left {}'s {} in Steam as it is: {}{} already holds earlier art", appid, slot,
                  stem, kBackup);
      }
      if (extension.empty() || !clear) {
        RestoreSlot(grid / stem);
        continue;
      }
      const fs::path file = grid / (stem + extension);
      std::error_code ec;
      fs::create_directories(grid, ec);
      fs::copy_file(pick->second, file, fs::copy_options::overwrite_existing, ec);
      if (ec) {
        failure = Err("steam_art_write_failed",
                      std::format("could not write {}: {}", file.string(), ec.message()))
                      .error();
        continue;
      }
      written.insert(file.lexically_relative(userdata).generic_string());
    }
  }
  if (auto saved = SaveWritten(written_file, written); !saved) return saved;
  if (failure) return std::unexpected(*failure);
  return {};
}

Result<void> RemoveGridArt(const fs::path& steam_root, const fs::path& written_file) {
  const std::set<std::string> written = ReadWritten(written_file);
  std::error_code ec;
  for (const std::string& key : written) fs::remove(steam_root / "userdata" / key, ec);
  for (const std::string& key : written) {
    RestoreSlot((steam_root / "userdata" / key).replace_extension());
  }
  return SaveWritten(written_file, {});
}

}  // namespace mira::steam
