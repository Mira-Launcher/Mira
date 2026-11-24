#include "library/FoundArchives.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <fstream>
#include <set>

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "library/Drops.h"
#include "library/Import.h"
#include "runner/Exec.h"

namespace mira::library {
namespace {
namespace fs = std::filesystem;
using nlohmann::json;

constexpr std::array<std::string_view, 9> kExtractors = {
    "7z", "7zz", "7za", "unrar", "bsdtar", "unzip", "ark", "file-roller", "tar"};

// Read to its end: /proc files say they're empty.
std::string ReadProc(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

int ParentOf(int pid) {
  const std::string stat = ReadProc(std::format("/proc/{}/stat", pid));
  // "pid (name) state ppid", and a name may hold anything.
  const std::size_t end = stat.rfind(") ");
  int parent = 0;
  if (end != std::string::npos && end + 4 < stat.size())
    std::from_chars(stat.data() + end + 4, stat.data() + stat.size(), parent);
  return parent;
}

bool IsOurs(int pid) {
  for (int step = 0; pid > 1 && step < 8; ++step, pid = ParentOf(pid)) {
    if (pid == ::getpid()) return true;
  }
  return false;
}

// Flags and position of one open file.
std::pair<int, std::uint64_t> FdInfo(const fs::path& proc, const fs::path& fd) {
  std::ifstream info(proc / "fdinfo" / fd.filename());
  std::string key;
  std::string value;
  int flags = 0;
  std::uint64_t position = 0;
  while (info >> key >> value) {
    if (key == "pos:") std::from_chars(value.data(), value.data() + value.size(), position);
    if (key == "flags:") {
      std::from_chars(value.data(), value.data() + value.size(), flags, 8);
      break;
    }
  }
  return {flags, position};
}

}  // namespace

json FoundArchives::ToJson(const FoundArchive& archive) {
  const std::vector<fs::path> volumes = VolumesOf(archive.path);
  std::uintmax_t bytes = 0;
  for (const fs::path& volume : volumes) {
    std::error_code ec;
    if (const std::uintmax_t size = fs::file_size(volume, ec); !ec) bytes += size;
  }
  return {{"path", archive.path.string()},
          {"name", archive.path.filename().string()},
          {"bytes", bytes},
          {"volumes", volumes.size()},
          {"root", archive.root.string()},
          {"taken_by", archive.taken_by}};
}

void FoundArchives::Offer(FoundArchive archive) {
  archive.path = archive.path.lexically_normal();
  {
    const std::lock_guard lock(mutex_);
    const auto known = std::ranges::find(archives_, archive.path, &FoundArchive::path);
    if (known != archives_.end()) {
      if (known->taken_by == archive.taken_by) return;
      *known = archive;
    } else {
      archives_.push_back(archive);
    }
  }
  events_.Publish("library.archive_found", ToJson(archive));
}

std::optional<FoundArchive> FoundArchives::Take(const fs::path& path) {
  std::optional<FoundArchive> taken = Find(path);
  if (!taken) return std::nullopt;
  {
    const std::lock_guard lock(mutex_);
    if (std::erase_if(archives_, [&](const FoundArchive& archive) {
          return archive.path == taken->path;
        }) == 0)
      return std::nullopt;
  }
  events_.Publish("library.archive_settled", {{"path", taken->path.string()}});
  return taken;
}

void FoundArchives::Appeared(const fs::path& appeared) {
  for (const FoundArchive& archive : All()) {
    // While another app holds it, the watcher sees it through.
    if (archive.path.parent_path() == appeared.parent_path() && archive.taken_by.empty() &&
        std::ranges::contains(archive.outputs, appeared))
      Take(archive.path);
  }
}

std::vector<FoundArchive> FoundArchives::All() const {
  const std::lock_guard lock(mutex_);
  return archives_;
}

std::optional<FoundArchive> FoundArchives::Find(const fs::path& path) const {
  const std::lock_guard lock(mutex_);
  const auto found = std::ranges::find(archives_, path.lexically_normal(), &FoundArchive::path);
  return found == archives_.end() ? std::nullopt : std::optional(*found);
}

std::optional<Holder> HolderOf(const fs::path& archive, const std::vector<fs::path>& outputs) {
  const std::vector<fs::path> volumes = VolumesOf(archive);
  // By inode: a sandboxed app sees the file under another path.
  std::set<std::pair<dev_t, ino_t>> wanted;
  for (const fs::path& volume : volumes) {
    struct stat info{};
    if (::stat(volume.c_str(), &info) == 0) wanted.emplace(info.st_dev, info.st_ino);
  }
  std::optional<Holder> holder;
  std::error_code ec;
  // As /proc spells them.
  std::vector<fs::path> real;
  for (const fs::path& output : outputs) real.push_back(fs::weakly_canonical(output, ec));
  for (const auto& proc : fs::directory_iterator("/proc", ec)) {
    const std::string name = proc.path().filename().string();
    int pid = 0;
    const auto parsed = std::from_chars(name.data(), name.data() + name.size(), pid);
    if (parsed.ec != std::errc() || parsed.ptr != name.data() + name.size()) continue;

    std::string comm = ReadProc(proc.path() / "comm");
    if (comm.ends_with('\n')) comm.pop_back();
    std::string cmdline = ReadProc(proc.path() / "cmdline");
    std::ranges::replace(cmdline, '\0', ' ');
    const bool jdownloader =
        comm.starts_with("java") && cmdline.find("JDownloader") != std::string::npos;
    const bool extractor = jdownloader || std::ranges::contains(kExtractors, comm);

    bool open = false;
    bool writes = false;
    bool in_output = false;
    std::uint64_t position = 0;
    std::error_code fd_ec;
    // Stepped with an error code: the process may end meanwhile.
    for (fs::directory_iterator it(proc.path() / "fd", fd_ec), end; !fd_ec && it != end;
         it.increment(fd_ec)) {
      const fs::directory_entry& fd = *it;
      struct stat info{};
      if (::stat(fd.path().c_str(), &info) != 0) continue;
      if (wanted.contains({info.st_dev, info.st_ino})) {
        const auto [flags, at] = FdInfo(proc.path(), fd.path());
        open = true;
        writes = writes || (flags & O_ACCMODE) != O_RDONLY;
        position += at;
      } else if (extractor && S_ISREG(info.st_mode)) {
        std::error_code link_ec;
        const fs::path target = fs::read_symlink(fd.path(), link_ec);
        in_output = in_output || (!link_ec && paths::IsWithin(target, real));
      }
    }
    if (extractor && !in_output) {
      std::error_code link_ec;
      const fs::path cwd = fs::read_symlink(proc.path() / "cwd", link_ec);
      in_output = !link_ec && paths::IsWithin(cwd, real, /*allow_equal=*/true);
    }
    // Started on it, though it may not have it open this instant.
    const bool named = extractor && cmdline.find(archive.filename().string()) != std::string::npos;
    if ((!open && !in_output && !named) || IsOurs(pid)) continue;
    const bool certain = writes || extractor;
    if (!holder || (certain && !holder->certain)) {
      holder = Holder{
          .name = jdownloader ? "JDownloader" : comm, .position = position, .certain = certain};
    }
  }
  return holder;
}

std::vector<fs::path> OutputsOf(const fs::path& archive) {
  const fs::path dir = archive.parent_path();
  std::set<fs::path> outputs = {dir / StemWithoutArchiveExtension(archive)};
  // Listing a tarball unpacks it, too slow for this.
  if (const auto listing = runner::IsTarball(archive) ? Result<ArchiveListing>(ArchiveListing{})
                                                      : ListArchive(archive, {})) {
    for (const ArchiveEntry& entry : listing->entries) {
      if (!entry.path.empty()) outputs.insert(dir / *entry.path.begin());
    }
  }
  return {outputs.begin(), outputs.end()};
}

Result<fs::path> ExtractFound(const config::Config& config, const FoundArchive& archive,
                              const ExtractOptions& options,
                              const std::function<void(int percent)>& on_progress) {
  const fs::path dir = archive.path.parent_path();
  const std::string name = StemWithoutArchiveExtension(archive.path).string();
  const fs::path work =
      dir / std::format("{}{}", kExtractingPrefix, archive.path.filename().string());
  std::error_code ec;
  fs::remove_all(work, ec);
  const fs::path staged = work / name;
  fs::create_directories(staged, ec);
  if (ec) return Err("dest_dir_failed", ec.message());
  const auto give_up = [&](Error error) -> Result<fs::path> {
    std::error_code clean;
    fs::remove_all(work, clean);
    return std::unexpected(std::move(error));
  };

  if (auto done = runner::Extract(archive.path, staged, options.password, on_progress); !done)
    return give_up(done.error());
  RemoveEscapingLinks(staged);

  fs::path placed;
  if (archive.drop && IsProgram(config, staged)) {
    const ImportKind kind =
        options.choices.app.value_or(false) ? ImportKind::kApp : ImportKind::kGame;
    auto imported = ImportUnderFreeName(config, staged, kind);
    if (!imported) return give_up(imported.error());
    placed = *imported;
    ConfirmWhenFound(placed, options.choices);
  } else {
    placed = FreeName(dir / name);
    // Before the move: a scan may find it at once.
    if (!archive.drop) ConfirmWhenFound(placed, options.choices);
    fs::rename(staged, placed, ec);
    if (ec) {
      TakeConfirmed(placed);
      return give_up(Error{"move_failed", ec.message(), "", {}});
    }
  }
  fs::remove_all(work, ec);

  if (options.delete_archive) {
    // The game is in place now, so a volume left behind is only logged.
    for (const fs::path& volume : VolumesOf(archive.path)) {
      if (!fs::remove(volume, ec) && ec)
        log::Warn("couldn't remove {}: {}", volume.string(), ec.message());
    }
  }
  return placed;
}

}  // namespace mira::library
