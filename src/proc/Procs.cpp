#include "proc/Procs.h"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>

#ifdef __APPLE__
#include <libproc.h>
#include <mach-o/dyld.h>
#include <sys/sysctl.h>

#include <cstring>
#endif

namespace mira::proc {
namespace {

#ifdef __linux__
std::optional<std::string> ReadProcFile(pid_t pid, const char* name) {
  std::ifstream file(std::format("/proc/{}/{}", pid, name), std::ios::binary);
  if (!file) return std::nullopt;
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}
#endif

#ifdef __APPLE__
// KERN_PROCARGS2: argc, the exec path, padding, argv[0..argc), then the environment.
struct ProcArgs {
  std::string argv;
  std::string environ;
};

std::optional<ProcArgs> ReadProcArgs(pid_t pid) {
  int mib[3] = {CTL_KERN, KERN_PROCARGS2, pid};
  size_t size = 0;
  if (sysctl(mib, 3, nullptr, &size, nullptr, 0) != 0 || size < sizeof(int)) return std::nullopt;
  std::string buffer(size, '\0');
  if (sysctl(mib, 3, buffer.data(), &size, nullptr, 0) != 0) return std::nullopt;
  buffer.resize(size);
  int argc = 0;
  std::memcpy(&argc, buffer.data(), sizeof(argc));
  size_t at = buffer.find('\0', sizeof(argc));  // past the exec path
  if (at == std::string::npos) return std::nullopt;
  while (at < buffer.size() && buffer[at] == '\0') ++at;
  ProcArgs args;
  const size_t argv_start = at;
  for (int i = 0; i < argc && at < buffer.size(); ++i) at = buffer.find('\0', at) + 1;
  if (at == 0 || at > buffer.size()) at = buffer.size();
  args.argv = buffer.substr(argv_start, at - argv_start);
  // The environment ends at the first empty string.
  if (at >= buffer.size() || buffer[at] == '\0') return args;
  const size_t env_end = buffer.find(std::string_view("\0\0", 2), at);
  args.environ =
      buffer.substr(at, env_end == std::string::npos ? std::string::npos : env_end + 1 - at);
  return args;
}
#endif

}  // namespace

std::vector<pid_t> AllPids() {
  std::vector<pid_t> pids;
#ifdef __APPLE__
  const int count = proc_listallpids(nullptr, 0);
  if (count <= 0) return pids;
  pids.resize(size_t(count) + 64);  // room for ones started in between
  const int got = proc_listallpids(pids.data(), int(pids.size() * sizeof(pid_t)));
  pids.resize(got > 0 ? size_t(got) : 0);
  std::erase(pids, 0);
#else
  DIR* dir = ::opendir("/proc");
  if (!dir) return pids;
  while (const dirent* entry = ::readdir(dir)) {
    if (std::isdigit(static_cast<unsigned char>(entry->d_name[0])))
      pids.push_back(std::atoi(entry->d_name));
  }
  ::closedir(dir);
#endif
  return pids;
}

std::optional<std::string> Environ(pid_t pid) {
#ifdef __APPLE__
  auto args = ReadProcArgs(pid);
  if (!args) return std::nullopt;
  return std::move(args->environ);
#else
  return ReadProcFile(pid, "environ");
#endif
}

std::optional<std::string> Cmdline(pid_t pid) {
#ifdef __APPLE__
  auto args = ReadProcArgs(pid);
  if (!args) return std::nullopt;
  return std::move(args->argv);
#else
  return ReadProcFile(pid, "cmdline");
#endif
}

std::string Name(pid_t pid) {
#ifdef __APPLE__
  char name[2 * MAXCOMLEN + 1] = {};
  return proc_name(pid, name, sizeof(name)) > 0 ? name : "";
#else
  std::string name = ReadProcFile(pid, "comm").value_or("");
  while (!name.empty() && name.back() == '\n') name.pop_back();
  return name;
#endif
}

bool HasEntry(const std::string& environ, const std::string& entry) {
  for (size_t at = 0; at < environ.size();) {
    const size_t end = environ.find('\0', at);
    if (environ.compare(at, (end == std::string::npos ? environ.size() : end) - at, entry) == 0)
      return true;
    if (end == std::string::npos) break;
    at = end + 1;
  }
  return false;
}

std::vector<pid_t> WithEntry(const std::string& entry) {
  std::vector<pid_t> found;
  for (const pid_t pid : AllPids()) {
    if (const auto environ = Environ(pid); environ && HasEntry(*environ, entry))
      found.push_back(pid);
  }
  return found;
}

bool AnyOpenForWriting(const std::set<std::string>& files) {
#ifdef __APPLE__
  for (const pid_t pid : AllPids()) {
    const int bytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
    if (bytes <= 0) continue;
    std::vector<proc_fdinfo> fds(size_t(bytes) / sizeof(proc_fdinfo) + 16);
    const int got =
        proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds.data(), int(fds.size() * sizeof(proc_fdinfo)));
    for (int i = 0; i < got / int(sizeof(proc_fdinfo)); ++i) {
      if (fds[size_t(i)].proc_fdtype != PROX_FDTYPE_VNODE) continue;
      vnode_fdinfowithpath info{};
      if (proc_pidfdinfo(pid, fds[size_t(i)].proc_fd, PROC_PIDFDVNODEPATHINFO, &info,
                         sizeof(info)) <= 0)
        continue;
      // fi_openflags holds the kernel's FREAD (1) and FWRITE (2).
      if ((info.pfi.fi_openflags & 2) != 0 && files.contains(info.pvip.vip_path)) return true;
    }
  }
#else
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
    const std::string pid = entry.path().filename().string();
    if (pid.empty() || !std::isdigit(static_cast<unsigned char>(pid[0]))) continue;
    std::error_code fd_ec;
    for (const auto& fd : std::filesystem::directory_iterator(entry.path() / "fd", fd_ec)) {
      std::error_code link_ec;
      const std::filesystem::path target = std::filesystem::read_symlink(fd.path(), link_ec);
      if (link_ec || !files.contains(target.string())) continue;
      std::ifstream info(entry.path() / "fdinfo" / fd.path().filename());
      std::string key;
      std::string flags;
      while (info >> key >> flags) {
        if (key != "flags:") continue;
        int mode = 0;
        std::from_chars(flags.data(), flags.data() + flags.size(), mode, 8);
        if ((mode & O_ACCMODE) != O_RDONLY) return true;
        break;
      }
    }
  }
#endif
  return false;
}

std::filesystem::path SelfExe() {
#ifdef __APPLE__
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string path(size, '\0');
  if (_NSGetExecutablePath(path.data(), &size) != 0) return {};
  path.resize(std::strlen(path.c_str()));
  std::error_code ec;
  const auto resolved = std::filesystem::canonical(path, ec);
  return ec ? std::filesystem::path(path) : resolved;
#else
  std::error_code ec;
  return std::filesystem::read_symlink("/proc/self/exe", ec);
#endif
}

}  // namespace mira::proc
