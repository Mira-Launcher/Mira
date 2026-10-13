#include "launchers/Autodesk.h"

#include <format>
#include <fstream>
#include <string>

#include "core/Strings.h"

namespace mira::launchers::autodesk {
namespace fs = std::filesystem;

bool IsCreateInstaller(const fs::path& file, std::string_view code) {
  const std::string name = strings::ToLower(file.filename().string());
  return name.starts_with(strings::ToLower(std::format("create_installer_{}_", code))) &&
         name.ends_with(".exe");
}

std::optional<fs::path> FindImage(const fs::path& prefix, fs::file_time_type since) {
  std::error_code ec;
  std::optional<fs::path> newest;
  fs::file_time_type newest_time = since;
  // Downloads/Autodesk/<Product> - (EN)/Setup.exe, with " (1)" added when that folder is taken.
  for (const auto& entry :
       fs::directory_iterator(prefix / "drive_c/users/steamuser/Downloads/Autodesk", ec)) {
    const fs::path setup = entry.path() / "Setup.exe";
    if (!fs::is_regular_file(setup, ec) || !fs::is_regular_file(entry.path() / "setup.xml", ec))
      continue;
    // The folder's time: the image's files keep their times from Autodesk's archive.
    const auto time = fs::last_write_time(entry.path(), ec);
    if (!ec && time >= newest_time) {
      newest = setup;
      newest_time = time;
    }
  }
  return newest;
}

namespace {

fs::path DeployLog(const fs::path& prefix) {
  return prefix / "drive_c/users/steamuser/AppData/Local/Temp/AdOdisDeployTool.log";
}

}  // namespace

bool ImageBuilt(const fs::path& prefix) {
  std::ifstream log(DeployLog(prefix));
  // Its last line; the "Installer created successfully" dialog follows.
  for (std::string line; std::getline(log, line);) {
    if (line.contains("Finalize download-now image")) return true;
  }
  return false;
}

void ForgetImageBuilt(const fs::path& prefix) {
  std::error_code ec;
  fs::remove(DeployLog(prefix), ec);
}

Result<void> PrepareImage(const fs::path& setup) {
  // Setup.exe imports its DLLs from the side-by-side assemblies in ODIS/, found on Windows through
  // Setup.exe.config's <probing privatePath="ODIS"/>, which Wine ignores: it looks beside the exe.
  const fs::path image = setup.parent_path();
  for (const char* assembly : {"odis.bs.win", "odis.bs.wx"}) {
    std::error_code ec;
    if (fs::exists(image / assembly, ec)) continue;
    fs::copy(image / "ODIS" / assembly, image / assembly, fs::copy_options::recursive, ec);
    if (ec) {
      return Err("install_failed",
                 std::format("couldn't ready the Autodesk install image: {}", ec.message()));
    }
  }
  return {};
}

}  // namespace mira::launchers::autodesk
