#include "library/ArchiveExtractor.h"

#include <fcntl.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <fstream>
#include <optional>
#include <regex>
#include <set>
#include <string>

#include "core/Strings.h"
#include "runner/Exec.h"

namespace mira::library {
namespace {
namespace fs = std::filesystem;

constexpr std::array kSuffixes = {std::string_view{".tar.gz"}, std::string_view{".tar.xz"},
                                  std::string_view{".tar.bz2"}, std::string_view{".tgz"},
                                  std::string_view{".tar"},     std::string_view{".zip"},
                                  std::string_view{".rar"},     std::string_view{".7z"},
                                  std::string_view{".7z.001"},  std::string_view{".zip.001"}};

// The longest matching suffix, or empty.
std::string_view SuffixOf(const fs::path& path) {
  const std::string name = strings::ToLower(path.filename().string());
  std::string_view best;
  for (std::string_view suffix : kSuffixes) {
    if (name.ends_with(suffix) && suffix.size() > best.size()) best = suffix;
  }
  return best;
}

// One file of a split archive: `set` is shared by every volume of the same
// archive, `index` is 1 for the volume 7-Zip is pointed at.
struct Volume {
  std::string set;
  int index = 0;
};

// Game.part1.rar (new-style RAR), Game.rar + Game.r00 (old-style RAR) and
// Game.7z.001 / Game.zip.001 (split files).
std::optional<Volume> VolumeOf(const fs::path& path) {
  static const std::regex kRarPart(R"(^(.*)\.part0*([0-9]{1,6})\.rar$)");
  static const std::regex kRarOld(R"(^(.*)\.r([0-9]{2,3})$)");
  static const std::regex kSplit(R"(^(.*\.(?:7z|zip))\.([0-9]{3})$)");
  const std::string name = strings::ToLower(path.filename().string());
  std::smatch m;
  if (std::regex_match(name, m, kRarPart)) return Volume{m[1].str() + ".part", std::stoi(m[2])};
  if (std::regex_match(name, m, kSplit)) return Volume{m[1].str() + ".split", std::stoi(m[2])};
  if (std::regex_match(name, m, kRarOld)) return Volume{m[1].str() + ".rar", std::stoi(m[2]) + 2};
  if (name.ends_with(".rar")) return Volume{name, 1};
  return std::nullopt;
}

// `dest`, or the first of "<dest> (2)", "<dest> (3)"... that's free: an archive never replaces a folder already
// there, such as a game with its saves.
fs::path FreeName(const fs::path& dest) {
  std::error_code ec;
  fs::path name = dest;
  for (int n = 2; fs::exists(name, ec); ++n) name = dest.parent_path() / std::format("{} ({})", dest.filename().string(), n);
  return name;
}

}  // namespace

bool LooksLikeArchive(const fs::path& path) { return !SuffixOf(path).empty() && !IsLaterVolume(path); }

bool IsLaterVolume(const fs::path& path) {
  const std::optional<Volume> volume = VolumeOf(path);
  return volume && volume->index > 1;
}

std::vector<fs::path> VolumesOf(const fs::path& archive) {
  const std::optional<Volume> volume = VolumeOf(archive);
  if (!volume) return {archive};
  std::vector<std::pair<int, fs::path>> found;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(archive.parent_path(), ec)) {
    const std::optional<Volume> other = VolumeOf(entry.path());
    if (other && other->set == volume->set) found.emplace_back(other->index, entry.path());
  }
  std::ranges::sort(found);
  std::vector<fs::path> volumes;
  for (auto& [index, path] : found) volumes.push_back(std::move(path));
  return volumes;
}

std::optional<fs::path> FirstVolumeOf(const fs::path& later) {
  const std::vector<fs::path> volumes = VolumesOf(later);
  if (volumes.empty() || IsLaterVolume(volumes.front())) return std::nullopt;
  return volumes.front();
}

bool AnyOpenForWriting(const std::vector<fs::path>& paths) {
  std::set<std::string> wanted;
  for (const fs::path& path : paths) {
    std::error_code ec;
    const fs::path canonical = fs::canonical(path, ec);
    if (!ec) wanted.insert(canonical.string());
  }
  if (wanted.empty()) return false;

  std::error_code ec;
  for (const auto& proc : fs::directory_iterator("/proc", ec)) {
    const std::string pid = proc.path().filename().string();
    if (pid.empty() || !std::ranges::all_of(pid, ::isdigit)) continue;
    std::error_code fd_ec;
    for (const auto& fd : fs::directory_iterator(proc.path() / "fd", fd_ec)) {
      std::error_code link_ec;
      const fs::path target = fs::read_symlink(fd.path(), link_ec);
      if (link_ec || !wanted.contains(target.string())) continue;
      std::ifstream info(proc.path() / "fdinfo" / fd.path().filename());
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
  return false;
}

fs::path StemWithoutArchiveExtension(const fs::path& archive) {
  const std::string name = archive.filename().string();
  static const std::regex kPart(R"(\.part[0-9]+\.rar$)", std::regex::icase);
  std::smatch match;
  if (std::regex_search(name, match, kPart)) return name.substr(0, match.position(0));
  return name.substr(0, name.size() - SuffixOf(archive).size());
}

Result<void> ExtractAndRemove(const fs::path& archive, const fs::path& dest_dir) {
  const std::string_view suffix = SuffixOf(archive);
  if (suffix.empty() || IsLaterVolume(archive)) return Err("not_an_archive", "unrecognized archive extension");

  const fs::path work_dir = dest_dir.parent_path() / std::format("{}{}", kExtractingPrefix, dest_dir.filename().string());
  std::error_code ec;
  fs::remove_all(work_dir, ec);
  fs::create_directories(work_dir, ec);
  if (ec) return Err("dest_dir_failed", ec.message());

  const Result<void> extracted = runner::Extract(archive, work_dir);
  if (!extracted) {
    fs::remove_all(work_dir, ec);
    return extracted;
  }

  fs::rename(work_dir, FreeName(dest_dir), ec);
  if (ec) {
    fs::remove_all(work_dir, ec);
    return Err("move_failed", ec.message());
  }

  for (const fs::path& volume : VolumesOf(archive)) {
    fs::remove(volume, ec);
    if (ec) return Err("archive_cleanup_failed", ec.message());
  }
  return {};
}

}  // namespace mira::library
