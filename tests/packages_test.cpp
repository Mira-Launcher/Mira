#include <doctest.h>

#include "system/Packages.h"

using namespace mira;

TEST_CASE("Distro families come from os-release, with ostree and SteamOS ahead of ID_LIKE") {
  CHECK(system::ParseOsRelease("ID=cachyos\nID_LIKE=arch\n", false).family == system::Family::Arch);
  CHECK(system::ParseOsRelease("ID=pop\nID_LIKE=\"ubuntu debian\"\n", false).family == system::Family::Debian);
  CHECK(system::ParseOsRelease("ID=bazzite\nID_LIKE=\"fedora\"\n", true).family == system::Family::Ostree);
  CHECK(system::ParseOsRelease("ID=steamos\nID_LIKE=arch\n", false).family == system::Family::SteamOS);
  CHECK(system::ParseOsRelease("ID=nixos\n", false).family == system::Family::Unknown);
}

TEST_CASE("Install commands per family; none where Mira can't install") {
  CHECK(system::InstallCommand(system::Family::Arch, {"cabextract"}) ==
        std::vector<std::string>{"pacman", "-S", "--needed", "--noconfirm", "cabextract"});
  CHECK(system::InstallCommand(system::Family::Ostree, {"cabextract"}).front() == "rpm-ostree");
  CHECK(system::InstallCommand(system::Family::SteamOS, {"cabextract"}).empty());
  CHECK(system::InstallCommand(system::Family::Fedora, {}).empty());
  CHECK_FALSE(system::ToolsFor("winetricks").empty());
  CHECK(system::ToolsFor("nonsense").empty());
}
