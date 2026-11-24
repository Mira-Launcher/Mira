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

#include "core/Log.h"
#include "core/Strings.h"
#include "runner/Exec.h"
#include "system/Packages.h"

namespace mira::library {
namespace {
namespace fs = std::filesystem;

constexpr std::array kSuffixes = {std::string_view{".tar.gz"},  std::string_view{".tar.xz"},
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

// `dest`, or the first of "<dest> (2)", "<dest> (3)"... that's free: an archive never replaces a
// folder already there, such as a game with its saves.
}  // namespace

fs::path FreeName(const fs::path& dest) {
  std::error_code ec;
  fs::path name = dest;
  for (int n = 2; fs::exists(name, ec); ++n)
    name = dest.parent_path() / std::format("{} ({})", dest.filename().string(), n);
  return name;
}

Result<std::optional<std::string>> FindPassword(const fs::path& archive,
                                                const std::vector<std::string>& candidates) {
  if (runner::IsTarball(archive)) return std::string();
  auto listing = ListArchive(archive, candidates);
  if (!listing) return std::unexpected(listing.error());
  return std::move(listing->password);
}

Result<ArchiveListing> ListArchive(const fs::path& archive,
                                   const std::vector<std::string>& candidates) {
  ArchiveListing listing;
  constexpr int kTimeoutSeconds = 60;
  if (runner::IsTarball(archive)) {
    Command command;
    command.timeout_s = kTimeoutSeconds;  // it's unpacked whole to be listed
    command.argv = {"tar", "-tf", archive.string()};
    const auto listed = runner::RunAndWait(command);
    listing.password = std::string();
    if (!listed && listed.error().code == "exec_timeout") return listing;  // extracted unseen
    if (!listed) return std::unexpected(listed.error());
    if (listed->exit_code != 0)
      return Err("extract_failed",
                 std::format("tar can't read {}: {}", archive.filename().string(), listed->output));
    for (std::string line : strings::Split(listed->output, '\n')) {
      if (line.starts_with("./")) line.erase(0, 2);
      if (!line.empty() && !line.ends_with('/')) listing.entries.push_back({line});
    }
    return listing;
  }
  const std::optional<std::string> seven_zip = runner::SevenZip();
  if (!seven_zip) {
    // Its fix installs 7-Zip, as for any other tool Mira needs.
    if (auto ready = system::RequireFeature("archives", "Extracting archives"); !ready)
      return std::unexpected(ready.error());
    return Err("extractor_missing", "install 7zip to extract archives");
  }
  const auto run = [&](std::vector<std::string> args) {
    Command command;
    if (args.front() == "l") command.timeout_s = kTimeoutSeconds;
    command.argv = {*seven_zip};
    command.argv.insert(command.argv.end(), args.begin(), args.end());
    return runner::RunAndWait(command);
  };
  // Always given a password: without one 7-Zip stops to ask for it.
  const auto list = [&](const std::string& password) {
    return run({"l", "-slt", "-p" + password, archive.string()});
  };
  auto listed = list("mira-no-password");
  std::optional<std::string> names_password;
  if (!listed) return std::unexpected(listed.error());
  if (listed->exit_code != 0) {
    // The names are encrypted too, so listing is the test.
    if (listed->output.find("rong password") == std::string::npos &&
        listed->output.find("Enter password") == std::string::npos)
      return Err("extract_failed", std::format("7-Zip can't read {}: {}",
                                               archive.filename().string(), listed->output));
    for (const std::string& candidate : candidates) {
      auto attempt = list(candidate);
      if (!attempt || attempt->exit_code != 0) continue;
      names_password = candidate;
      listed = std::move(attempt);
      break;
    }
    if (!names_password) return listing;
  }

  // Only the contents are encrypted: testing the smallest file is enough, and quick.
  std::string smallest;
  std::uintmax_t smallest_size = UINTMAX_MAX;
  std::string path;
  std::uintmax_t size = 0;
  bool encrypted = false;
  bool encrypted_empty = false;
  bool folder = false;
  bool in_entries = false;
  const auto keep = [&] {
    if (in_entries && !path.empty() && !folder) listing.entries.push_back({path, size});
  };
  std::istringstream lines(listed->output);
  for (std::string line; std::getline(lines, line);) {
    if (line.ends_with('\r')) line.pop_back();
    if (line.starts_with("----------")) {
      in_entries = true;  // the archive's own properties come before
      path.clear();
    } else if (line.starts_with("Path = ")) {
      keep();
      path = line.substr(7);
      size = 0;
      encrypted = false;
      folder = false;
    } else if (line.starts_with("Size = ")) {
      std::from_chars(line.data() + 7, line.data() + line.size(), size);
    } else if (line == "Folder = +" || line.starts_with("Attributes = D")) {
      folder = true;
    } else if (line == "Encrypted = +") {
      encrypted = true;
    }
    if (encrypted && size == 0) encrypted_empty = true;
    if (encrypted && size > 0 && size < smallest_size) {
      smallest = path;
      smallest_size = size;
    }
  }
  keep();
  if (names_password || (smallest.empty() && !encrypted_empty)) {
    listing.password = names_password.value_or("");
    return listing;
  }
  const auto opens = [&](const std::string& password) {
    std::vector<std::string> args = {"t", "-p" + password, archive.string()};
    if (!smallest.empty()) args.insert(args.end(), {"--", smallest});
    const auto attempt = run(std::move(args));
    return attempt && attempt->exit_code == 0;
  };
  // An empty file may or may not hold encrypted data, so only the test can say.
  if (smallest.empty() && opens("mira-no-password")) {
    listing.password = std::string();
    return listing;
  }
  for (const std::string& candidate : candidates) {
    if (!opens(candidate)) continue;
    listing.password = candidate;
    break;
  }
  return listing;
}

void RemoveEscapingLinks(const fs::path& dir) {
  std::error_code ec;
  const fs::path root = fs::weakly_canonical(dir, ec);
  if (ec) return;
  std::vector<fs::path> escaping;
  // Stepped with an error code: a folder it can't read must not throw.
  for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec),
       end;
       !ec && it != end; it.increment(ec)) {
    const fs::directory_entry& entry = *it;
    std::error_code link_ec;
    if (!entry.is_symlink(link_ec)) continue;
    // Resolved fully, so a chain of links can't step out where each one alone looks fine.
    const fs::path target = fs::weakly_canonical(entry.path(), link_ec);
    const fs::path inside = target.lexically_relative(root);
    if (link_ec || inside.empty() || *inside.begin() == "..") escaping.push_back(entry.path());
  }
  for (const fs::path& link : escaping) {
    log::Warn("removed a link leading out of an archive: {}", link.string());
    fs::remove(link, ec);
  }
}

bool LooksLikeArchive(const fs::path& path) {
  return !SuffixOf(path).empty() && !IsLaterVolume(path);
}

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
    // Stepped with an error code: the process may end meanwhile.
    for (fs::directory_iterator it(proc.path() / "fd", fd_ec), end; !fd_ec && it != end;
         it.increment(fd_ec)) {
      const fs::directory_entry& fd = *it;
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

}  // namespace mira::library
