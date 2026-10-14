#include "runner/WindowsTheme.h"

#include <cctype>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/Log.h"
#include "proc/ProcessSupervisor.h"
#include "runner/Exec.h"
#include "runner/Winetricks.h"

namespace mira::runner {
namespace {
namespace fs = std::filesystem;

constexpr std::string_view kPersonalize =
    "Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Themes\\\\Personalize]";

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
    case '1':
      return true;  // prefer dark
    case '2':
      return false;  // prefer light
    default:
      return std::nullopt;
  }
}

std::optional<bool> DesktopPrefersDark() {
  auto output = Read({"busctl", "--user", "call", "org.freedesktop.portal.Desktop",
                      "/org/freedesktop/portal/desktop", "org.freedesktop.portal.Settings",
                      "ReadOne", "ss", "org.freedesktop.appearance", "color-scheme"});
  if (!output) {
    output = Read({"gdbus", "call", "--session", "--dest", "org.freedesktop.portal.Desktop",
                   "--object-path", "/org/freedesktop/portal/desktop", "--method",
                   "org.freedesktop.portal.Settings.ReadOne", "org.freedesktop.appearance",
                   "color-scheme"});
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

std::string WithPrefixTheme(std::string_view user_reg, bool dark) {
  const std::string values = std::format(
      "\"AppsUseLightTheme\"=dword:0000000{0}\n\"SystemUsesLightTheme\"=dword:0000000{0}\n",
      dark ? 0 : 1);
  const size_t section = user_reg.find(kPersonalize);
  if (section == std::string_view::npos) {
    return std::format("{}\n[{} 0\n{}", user_reg, kPersonalize, values);
  }
  // The section's own lines, minus the two values, which go right after its header.
  const size_t body = user_reg.find('\n', section) + 1;
  size_t end = user_reg.find(
      "\n[", body - 1);  // from the header's own newline: the next line may be a header
  end = end == std::string_view::npos ? user_reg.size() : end + 1;
  std::string kept;
  std::istringstream lines{std::string(user_reg.substr(body, end - body))};
  for (std::string line; std::getline(lines, line);) {
    if (!line.starts_with("\"AppsUseLightTheme\"=") &&
        !line.starts_with("\"SystemUsesLightTheme\"="))
      kept += line + "\n";
  }
  return std::string(user_reg.substr(0, body)) + values + kept + std::string(user_reg.substr(end));
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
  // With nothing running, starting Wine to set it would also start the prefix's services, which
  // keep it running, and Proton's launch then waits for them to end.
  if (proc::FindPrefixProcesses(game.data_dir).empty()) {
    file.close();
    const fs::path target = fs::canonical(user_reg, ec);  // a link to pfx/user.reg stays a link
    if (ec)
      return Err("theme_failed",
                 std::format("couldn't resolve {}: {}", user_reg.string(), ec.message()));
    const fs::path temp = target.string() + ".mira-tmp";
    std::ofstream out(temp);
    out << WithPrefixTheme(text, *dark);
    out.close();
    if (!out) {
      fs::remove(temp, ec);
      return Err("theme_failed", std::format("couldn't write {}", temp.string()));
    }
    fs::rename(temp, target, ec);
    if (ec)
      return Err("theme_failed",
                 std::format("couldn't write {}: {}", target.string(), ec.message()));
    return {};
  }
  const std::string light = *dark ? "0" : "1";
  for (const char* value : {"AppsUseLightTheme", "SystemUsesLightTheme"}) {
    const auto ran = RunWine(
        runners, game,
        {"reg", "add", "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
         "/v", value, "/t", "REG_DWORD", "/d", light, "/f"});
    if (!ran) return std::unexpected(ran.error());
  }
  return {};
}

}  // namespace mira::runner
