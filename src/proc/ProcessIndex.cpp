#include "proc/ProcessIndex.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <unordered_set>

#include "core/Strings.h"
#include "model/Types.h"
#include "proc/Procs.h"

namespace mira::proc {
namespace {

constexpr std::int64_t kRereadSeconds = 5;

}  // namespace

std::string SteamLaunchAppId(const std::string& argv0, std::istream& rest) {
  if (!argv0.ends_with("/reaper")) return "";
  std::string arg;
  bool launch = false;
  while (std::getline(rest, arg, '\0') && arg != "--") {
    if (arg == "SteamLaunch") launch = true;
    if (launch && arg.starts_with("AppId=")) return arg.substr(6);
  }
  return "";
}

namespace {

ProcessInfo Read(pid_t pid) {
  ProcessInfo info;
  std::istringstream environ_file(Environ(pid).value_or(""));
  std::string item;
  while (std::getline(environ_file, item, '\0')) {
    if (item.starts_with("WINEPREFIX="))
      info.prefix = item.substr(11);
    else if (item.starts_with("STEAM_COMPAT_DATA_PATH=") && info.prefix.empty())
      info.prefix = item.substr(23);
  }
  std::istringstream cmdline_file(Cmdline(pid).value_or(""));
  std::getline(cmdline_file, info.argv0, '\0');
  info.steam_launch = SteamLaunchAppId(info.argv0, cmdline_file);
  if (info.prefix.empty()) {
    info.argv0.clear();  // only Wine processes are matched by path; keep the index small
    info.argv0.shrink_to_fit();
  }
  // Started by its host path, as Mira starts an app's exe, Wine keeps that path: matched as Windows
  // sees it.
  std::string prefix = info.prefix;
  while (prefix.ends_with('/')) prefix.pop_back();
  if (const std::string drive_c = prefix + "/drive_c/";
      !prefix.empty() && info.argv0.starts_with(drive_c)) {
    info.argv0 = "c:/" + info.argv0.substr(drive_c.size());
  }
  std::ranges::replace(info.argv0, '\\', '/');
  info.argv0 = strings::ToLower(info.argv0);
  return info;
}

}  // namespace

void ProcessIndex::Refresh() {
  const std::int64_t now = model::NowSeconds();
  std::unordered_set<pid_t> alive;
  for (const pid_t pid : AllPids()) {
    alive.insert(pid);
    const auto known = processes_.find(pid);
    if (known != processes_.end() && now - known->second.first_seen > kRereadSeconds) continue;
    ProcessInfo info = Read(pid);
    info.first_seen = known == processes_.end() ? now : known->second.first_seen;
    processes_[pid] = std::move(info);
  }
  std::erase_if(processes_, [&](const auto& item) { return !alive.contains(item.first); });
}

bool UnderWindowsPath(std::string_view argv0, std::string_view win_path) {
  return argv0.starts_with(win_path) &&
         (argv0.size() == win_path.size() || argv0[win_path.size()] == '/');
}

bool InPrefix(std::string_view value, std::string_view data_dir) {
  if (data_dir.empty()) return false;
  if (value == data_dir) return true;
  return value.size() > data_dir.size() && value.starts_with(data_dir) &&
         value[data_dir.size()] == '/';
}

}  // namespace mira::proc
