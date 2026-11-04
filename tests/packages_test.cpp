#include "system/Packages.h"

#include <doctest.h>

using namespace mira;

TEST_CASE("Distro families come from os-release, with ostree and SteamOS ahead of ID_LIKE") {
  CHECK(system::ParseOsRelease("ID=cachyos\nID_LIKE=arch\n", false).family == system::Family::Arch);
  CHECK(system::ParseOsRelease("ID=pop\nID_LIKE=\"ubuntu debian\"\n", false).family ==
        system::Family::Debian);
  CHECK(system::ParseOsRelease("ID=bazzite\nID_LIKE=\"fedora\"\n", true).family ==
        system::Family::Ostree);
  CHECK(system::ParseOsRelease("ID=steamos\nID_LIKE=arch\n", false).family ==
        system::Family::SteamOS);
  CHECK(system::ParseOsRelease("ID=nixos\n", false).family == system::Family::Unknown);
}

namespace {

// The family's own tool, the package last, and nothing that would stop to ask.
bool Runs(const std::vector<std::string>& command, std::string_view tool, std::string_view yes,
          std::string_view package) {
  return !command.empty() && command.front() == tool && command.back() == package &&
         std::ranges::find(command, yes) != command.end();
}

}  // namespace

TEST_CASE("A downloaded package installs by its path where Mira can update itself") {
  CHECK(Runs(system::InstallFileCommand(system::Family::Debian, "/tmp/m.deb"), "apt-get", "-y",
             "/tmp/m.deb"));
  CHECK(system::InstallFileCommand(system::Family::Fedora, "/tmp/m.rpm").back() == "/tmp/m.rpm");
  CHECK(system::InstallFileCommand(system::Family::Arch, "/tmp/m.deb").empty());
  CHECK(system::InstallFileCommand(system::Family::Ostree, "/tmp/m.rpm").empty());
}

TEST_CASE("Install commands per family; none where Mira can't install") {
  CHECK(Runs(system::InstallCommand(system::Family::Arch, {"cabextract"}), "pacman", "--noconfirm",
             "cabextract"));
  CHECK(system::InstallCommand(system::Family::Ostree, {"cabextract"}).front() == "rpm-ostree");
  CHECK(system::InstallCommand(system::Family::SteamOS, {"cabextract"}).empty());
  CHECK(system::InstallCommand(system::Family::Fedora, {}).empty());
  CHECK(Runs(system::UninstallCommand(system::Family::Debian, {"dolphin-emu"}), "apt-get", "-y",
             "dolphin-emu"));
  CHECK(system::UninstallCommand(system::Family::Unknown, {"dolphin-emu"}).empty());
  CHECK_FALSE(system::ToolsFor("winetricks").empty());
  CHECK(system::ToolsFor("nonsense").empty());
}
