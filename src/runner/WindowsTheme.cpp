#include "runner/WindowsTheme.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/Log.h"
#include "runner/Exec.h"
#include "runner/Winetricks.h"

namespace mira::runner {
namespace {
namespace fs = std::filesystem;

constexpr std::string_view kPersonalize = "Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Themes\\\\Personalize]";

std::optional<std::string> Read(std::vector<std::string> argv) {
  Command command;
  command.argv = std::move(argv);
  command.timeout_s = 2;
  const auto result = RunAndWait(command);
  if (!result || result->exit_code != 0) return std::nullopt;
  return result->output;
}

}  // namespace

std::optional<bool> ParseColorScheme(std::string_view output) {
  // The value is the last number: "v u 1" (busctl) or "(<<uint32 1>>,)" (gdbus).
  const size_t end = output.find_last_of("0123456789");
  if (end == std::string_view::npos) return std::nullopt;
  if (end > 0 && std::isdigit(static_cast<unsigned char>(output[end - 1]))) return std::nullopt;
  switch (output[end]) {
    case '1': return true;   // prefer dark
    case '2': return false;  // prefer light
    default: return std::nullopt;
  }
}

std::optional<bool> DesktopPrefersDark() {
  auto output = Read({"busctl", "--user", "call", "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                      "org.freedesktop.portal.Settings", "ReadOne", "ss", "org.freedesktop.appearance", "color-scheme"});
  if (!output) {
    output = Read({"gdbus", "call", "--session", "--dest", "org.freedesktop.portal.Desktop", "--object-path",
                   "/org/freedesktop/portal/desktop", "--method", "org.freedesktop.portal.Settings.ReadOne",
                   "org.freedesktop.appearance", "color-scheme"});
  }
  return output ? ParseColorScheme(*output) : std::nullopt;
}

std::optional<bool> PrefixPrefersDark(std::string_view user_reg) {
  const size_t section = user_reg.find(kPersonalize);
  if (section == std::string_view::npos) return std::nullopt;
  std::istringstream lines{std::string(user_reg.substr(user_reg.find('\n', section) + 1))};
  for (std::string line; std::getline(lines, line) && !line.starts_with('[');) {
    if (line.starts_with("\"AppsUseLightTheme\"=dword:")) return line.ends_with("00000000");
  }
  return std::nullopt;
}

Result<void> SyncWindowsTheme(const RunnerRegistry& runners, const model::Game& game) {
  const auto dark = DesktopPrefersDark();
  if (!dark || game.data_dir.empty()) return {};
  std::error_code ec;
  fs::path user_reg = fs::path(game.data_dir) / "user.reg";
  if (!fs::exists(user_reg, ec)) user_reg = fs::path(game.data_dir) / "pfx" / "user.reg";
  std::ifstream file(user_reg);
  if (!file) return {};  // no prefix yet
  const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (PrefixPrefersDark(text) == dark) return {};

  log::Info("setting {}'s Windows theme to {}", game.id, *dark ? "dark" : "light");
  const std::string light = *dark ? "0" : "1";
  for (const char* value : {"AppsUseLightTheme", "SystemUsesLightTheme"}) {
    const auto ran = RunWine(runners, game,
                             {"reg", "add", "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                              "/v", value, "/t", "REG_DWORD", "/d", light, "/f"});
    if (!ran) return std::unexpected(ran.error());
  }
  return {};
}

}  // namespace mira::runner
