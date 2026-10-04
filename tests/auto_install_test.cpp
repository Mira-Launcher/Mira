#include <doctest.h>

#include <filesystem>
#include <fstream>

#include "core/Strings.h"
#include "library/AutoInstall.h"
#include "runner/IRunner.h"

using namespace mira;
namespace fs = std::filesystem;

namespace {

fs::path TempFile(const char* name) {
  const fs::path dir = fs::temp_directory_path() / "mira-tests" / "auto-install";
  fs::create_directories(dir);
  const fs::path file = dir / name;
  fs::remove(file);
  return file;
}

void Write(const fs::path& path, std::string_view head, std::string_view tail, size_t padding) {
  std::ofstream out(path, std::ios::binary);
  out << head;
  out << std::string(padding, '\0');
  out << tail;
}

}  // namespace

TEST_CASE("A game installed elsewhere takes the installed folder's name unless it was renamed") {
  model::Game game;
  game.install_path = "/games/setup_clustertruck";
  game.name = strings::CleanGameName("setup_clustertruck");
  library::AdoptInstallFolder(game, "/prefixes/ct/drive_c/GOG Games/ClusterTruck");
  CHECK(game.name == strings::CleanGameName("ClusterTruck"));
  CHECK(game.install_path == "/prefixes/ct/drive_c/GOG Games/ClusterTruck");
  CHECK(game.installer_dir == "/games/setup_clustertruck");

  model::Game renamed;
  renamed.install_path = "/games/setup_clustertruck";
  renamed.name = "My Trucks";
  library::AdoptInstallFolder(renamed, "/prefixes/ct/drive_c/GOG Games/ClusterTruck");
  CHECK(renamed.name == "My Trucks");
}

TEST_CASE("DetectInstallerFormat recognizes Inno Setup near the head") {
  const fs::path file = TempFile("inno-head.exe");
  Write(file, "junk...Inno Setup junk", "", 0);
  CHECK(library::DetectInstallerFormat(file) == library::InstallerFormat::kInnoSetup);
}

TEST_CASE("DetectInstallerFormat recognizes NSIS's marker near the tail of a large file") {
  const fs::path file = TempFile("nsis-tail.exe");
  // Bigger than the 2MB sniff window on each end, marker only in the tail --
  // regression coverage for "don't just read the head of a monolithic
  // installer".
  Write(file, std::string(64, 'a'), "...Nullsoft...", 3 * 1024 * 1024);
  CHECK(library::DetectInstallerFormat(file) == library::InstallerFormat::kNsis);
}

TEST_CASE("DetectInstallerFormat reports unknown for a non-installer exe") {
  const fs::path file = TempFile("plain.exe");
  Write(file, "just a normal game binary, nothing special here", "", 0);
  CHECK(library::DetectInstallerFormat(file) == library::InstallerFormat::kUnknown);
}

TEST_CASE("MSI installers are detected by extension and run through msiexec") {
  const fs::path msi = TempFile("setup.msi");
  Write(msi, "Inno Setup", "", 0);
  CHECK(library::DetectInstallerFormat(msi) == library::InstallerFormat::kMsi);

  const fs::path text = TempFile("readme.txt");
  Write(text, "Inno Setup", "", 0);
  CHECK(library::DetectInstallerFormat(text) == library::InstallerFormat::kUnknown);

  CHECK(runner::WindowsProgram("/g/setup.msi") == std::vector<std::string>{"msiexec", "/i", "/g/setup.msi"});
  CHECK(runner::WindowsProgram("/g/start.BAT") == std::vector<std::string>{"cmd", "/c", "/g/start.BAT"});
  CHECK(runner::WindowsProgram("/g/game.exe") == std::vector<std::string>{"/g/game.exe"});
}
