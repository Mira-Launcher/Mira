#include "proc/ProcessIndex.h"

#include <dirent.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <unordered_set>

#include "model/Types.h"

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

ProcessInfo Read(const std::string& pid) {
  ProcessInfo info;
  std::ifstream environ_file("/proc/" + pid + "/environ", std::ios::binary);
  std::string item;
  while (std::getline(environ_file, item, '\0')) {
    if (item.starts_with("WINEPREFIX=")) info.prefix = item.substr(11);
    else if (item.starts_with("STEAM_COMPAT_DATA_PATH=") && info.prefix.empty()) info.prefix = item.substr(23);
  }
  std::ifstream cmdline_file("/proc/" + pid + "/cmdline", std::ios::binary);
  std::getline(cmdline_file, info.argv0, '\0');
  info.steam_launch = SteamLaunchAppId(info.argv0, cmdline_file);
  if (info.prefix.empty()) {
    info.argv0.clear();  // only Wine processes are matched by path; keep the index small
    info.argv0.shrink_to_fit();
  }
  std::ranges::replace(info.argv0, '\\', '/');
  for (char& ch : info.argv0) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return info;
}

}  // namespace

void ProcessIndex::Refresh() {
  const std::int64_t now = model::NowSeconds();
  std::unordered_set<pid_t> alive;
  DIR* proc_dir = ::opendir("/proc");
  if (!proc_dir) return;
  while (const dirent* entry = ::readdir(proc_dir)) {
    const std::string name = entry->d_name;
    if (name.empty() || !std::isdigit(static_cast<unsigned char>(name[0]))) continue;
    const pid_t pid = std::atoi(name.c_str());
    alive.insert(pid);
    const auto known = processes_.find(pid);
    if (known != processes_.end() && now - known->second.first_seen > kRereadSeconds) continue;
    ProcessInfo info = Read(name);
    info.first_seen = known == processes_.end() ? now : known->second.first_seen;
    processes_[pid] = std::move(info);
  }
  ::closedir(proc_dir);
  std::erase_if(processes_, [&](const auto& item) { return !alive.contains(item.first); });
}

bool UnderWindowsPath(std::string_view argv0, std::string_view win_path) {
  return argv0.starts_with(win_path) && (argv0.size() == win_path.size() || argv0[win_path.size()] == '/');
}

bool InPrefix(std::string_view value, std::string_view data_dir) {
  if (data_dir.empty()) return false;
  if (value == data_dir) return true;
  return value.size() > data_dir.size() && value.starts_with(data_dir) && value[data_dir.size()] == '/';
}

}  // namespace mira::proc
