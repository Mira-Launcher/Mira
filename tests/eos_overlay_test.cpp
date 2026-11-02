#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "epic/EosOverlay.h"
#include "epic/Legendary.h"
#include "support/TestEnv.h"

using namespace mira;
namespace fs = std::filesystem;

namespace {

std::string Read(const fs::path& path) {
  std::ifstream file(path);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

constexpr std::string_view kUserReg =
    "WINE REGISTRY Version 2\n"
    ";; All keys relative to \\\\User\\\\S-1-5-21-0-0-0-1000\n"
    "\n"
    "[Software\\\\Wine\\\\Drivers] 1761354000\n"
    "#time=1db45a1e0f00000\n"
    "\"Audio\"=\"pulse\"\n";

}  // namespace

TEST_CASE("WithOverlayPath adds the EOS key once and points it at the overlay") {
  const std::string added = epic::WithOverlayPath(kUserReg, R"(Z:\home\me\eos-overlay)");
  CHECK(added == std::string(kUserReg) +
                     "\n[Software\\\\Epic Games\\\\EOS]\n"
                     "\"OverlayPath\"=\"Z:\\\\home\\\\me\\\\eos-overlay\"\n");
  CHECK(epic::WithOverlayPath(added, R"(Z:\home\me\eos-overlay)") == added);
}

TEST_CASE("WithOverlayPath replaces the path in a key Wine or legendary wrote") {
  const std::string reg = std::string(kUserReg) +
                          "\n[SOFTWARE\\\\Epic Games\\\\EOS] 1761354001\n"
                          "#time=1db45a1e0f00001\n"
                          "\"OverlayPath\"=\"Z:/old/eos-overlay\"\n"
                          "\"Other\"=\"kept\"\n"
                          "\n[Software\\\\Next] 1761354002\n";
  const std::string changed = epic::WithOverlayPath(reg, R"(Z:\new)");
  CHECK(changed.find("\"OverlayPath\"=\"Z:\\\\new\"\n\"Other\"=\"kept\"") != std::string::npos);
  CHECK(changed.find("Z:/old") == std::string::npos);
  CHECK(changed.find("[Software\\\\Next]") != std::string::npos);

  const std::string removed = epic::WithOverlayPath(reg, "");
  CHECK(removed.find("OverlayPath") == std::string::npos);
  CHECK(removed.find("\"Other\"=\"kept\"") != std::string::npos);
  CHECK(epic::WithOverlayPath(kUserReg, "") == kUserReg);
}

TEST_CASE("WithOverlayPath puts the path after the key's #time line when it adds one") {
  const std::string reg = std::string(kUserReg) +
                          "\n[Software\\\\Epic Games\\\\EOS] 1761354001\n"
                          "#time=1db45a1e0f00001\n";
  CHECK(epic::WithOverlayPath(reg, "Z:\\o").ends_with(
      "#time=1db45a1e0f00001\n\"OverlayPath\"=\"Z:\\\\o\"\n"));
}

TEST_CASE("SetOverlayIn writes a prefix's user.reg only when it changes and skips a new prefix") {
  const fs::path prefix = test::TempDir("eos-overlay-prefix");
  REQUIRE(epic::SetOverlayIn(prefix, "/home/me/eos-overlay").has_value());
  CHECK_FALSE(fs::exists(prefix / "user.reg"));

  test::Touch(prefix / "user.reg", kUserReg);
  REQUIRE(epic::SetOverlayIn(prefix, "/home/me/eos-overlay").has_value());
  CHECK(Read(prefix / "user.reg").find(R"("OverlayPath"="Z:\\home\\me\\eos-overlay")") !=
        std::string::npos);

  // The key stays, empty, as legendary leaves it.
  REQUIRE(epic::SetOverlayIn(prefix, "").has_value());
  CHECK(Read(prefix / "user.reg").find("OverlayPath") == std::string::npos);
  CHECK(Read(prefix / "user.reg").starts_with(kUserReg));
}

TEST_CASE("DetectOverlay finds Mira's copy with the version legendary recorded") {
  test::TestEnv env("eos-overlay-detect");
  CHECK_FALSE(epic::DetectOverlay(env.config).installed);

  const fs::path dir = epic::ManagedOverlayDir(env.config);
  test::Touch(dir / "EOSOVH-Win64-Shipping.dll");
  test::Touch(epic::LegendaryConfigDir() / "overlay_install.json",
              R"({"install_path": ")" + dir.string() +
                  R"(", "version": "1.3.7++EOSSDK+Release-Overlay-CL-57923510-Windows"})");
  const runner::ToolStatus overlay = epic::DetectOverlay(env.config);
  CHECK(overlay.installed);
  CHECK(overlay.source == "managed");
  CHECK(fs::path(overlay.path).parent_path() == dir);
  CHECK(overlay.version == "1.3.7");
  fs::remove(epic::LegendaryConfigDir() / "overlay_install.json");
}
