#include "library/ScriptPresets.h"

#include <unistd.h>

#include <algorithm>
#include <filesystem>

#include "config/Resolver.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "steam/SteamDetector.h"

namespace mira::library {

namespace {

// As POST /v1/steam/status does it: only while Steam runs, since the URL would start it.
constexpr std::string_view kIfSteamRuns =
    "kill -0 \"$(cat {steam_pid} 2>/dev/null)\" 2>/dev/null || exit 0\n";
constexpr std::string_view kIfPowerProfiles =
    "command -v powerprofilesctl >/dev/null || exit 0\nsaved={runtime}/power-profile\n";

}  // namespace

const std::vector<ScriptStep>& PreLaunchSteps() {
  static const std::vector<ScriptStep> steps = {
      {.id = "steam-invisible",
       .label = "Invisible on Steam",
       .doc = "Your Steam friends see you as offline. Does nothing while Steam isn't running.",
       .command = std::string(kIfSteamRuns) + "{steam} steam://friends/status/invisible",
       .pair = "steam-online"},
      {.id = "performance-profile",
       .label = "Performance power profile",
       .doc = "Switches to the Performance power profile. Needs power-profiles-daemon.",
       // The first game to start keeps the profile to go back to.
       .command = std::string(kIfPowerProfiles) +
                  "[ -e \"$saved\" ] || powerprofilesctl get >\"$saved\"\n"
                  "powerprofilesctl set performance",
       .pair = "restore-power-profile"},
  };
  return steps;
}

const std::vector<ScriptStep>& PostExitSteps() {
  static const std::vector<ScriptStep> steps = {
      {.id = "steam-online",
       .label = "Online on Steam",
       .doc = "Sets your Steam status to online. Does nothing while Steam isn't running.",
       .command = std::string(kIfSteamRuns) + "{steam} steam://friends/status/online",
       .pair = "steam-invisible"},
      {.id = "restore-power-profile",
       .label = "Back to the power profile you had",
       .doc = "Goes back to the power profile from before Performance power profile ran.",
       .command = std::string(kIfPowerProfiles) + "[ -e \"$saved\" ] || exit 0\n"
                                                  "powerprofilesctl set \"$(cat \"$saved\")\"\n"
                                                  "rm -f \"$saved\"",
       .pair = "performance-profile",
       .needs_pair = true},
  };
  return steps;
}

std::string OwnScript(std::string_view script) {
  const std::string text = strings::Trim(script);
  if (text.empty() || text.find('\n') != std::string::npos) return std::string(script);
  std::error_code ec;
  const std::filesystem::path file = paths::Expand(text);
  if (!std::filesystem::is_regular_file(file, ec)) return std::string(script);
  const std::string quoted = strings::ShellQuote(file.string());
  return ::access(file.c_str(), X_OK) == 0 ? quoted : "sh " + quoted;
}

LaunchScripts ResolveLaunchScripts(const config::Config& config, const config::Resolver& resolver,
                                   const std::filesystem::path& runtime_dir) {
  LaunchScripts scripts{OwnScript(resolver.GetString("launch.pre_script")),
                        OwnScript(resolver.GetString("launch.post_script"))};
  // In the table's order, which is how a settings screen shows them.
  const auto chosen = [&](std::string_view key, const std::vector<ScriptStep>& table) {
    const std::vector<std::string> ids = resolver.GetStringArray(key);
    std::vector<ScriptStep> steps;
    for (const ScriptStep& step : table) {
      if (std::ranges::contains(ids, step.id)) steps.push_back(step);
    }
    return steps;
  };
  std::vector<ScriptStep> pre = chosen("launch.pre_steps", PreLaunchSteps());
  std::vector<ScriptStep> post = chosen("launch.post_steps", PostExitSteps());
  if (pre.empty() && post.empty()) return scripts;

  std::string steam_command;
  for (const std::string& arg : steam::SteamCommand(config))
    steam_command += (steam_command.empty() ? "" : " ") + strings::ShellQuote(arg);
  const std::string steam_pid = strings::ShellQuote(steam::SteamPidFile(config).string());
  const std::string runtime = strings::ShellQuote(runtime_dir.string());
  const auto fill = [](std::string& step, std::string_view name, const std::string& value) {
    for (size_t at = step.find(name); at != std::string::npos;
         at = step.find(name, at + value.size())) {
      step.replace(at, name.size(), value);
    }
  };
  // Each part runs in a subshell, so an `exit` in one only ends that part. Only the game's own
  // pre-launch script can cancel the launch.
  const auto own = [](const std::string& script, std::string_view after) {
    return script.empty() ? std::string() : "(\n" + script + "\n)" + std::string(after) + "\n";
  };
  const auto append = [&](std::string& script, std::vector<ScriptStep>& steps) {
    for (ScriptStep& step : steps) {
      fill(step.command, "{steam_pid}", steam_pid);
      fill(step.command, "{runtime}", runtime);
      fill(step.command, "{steam}", steam_command);
      script += "(\n" + step.command + "\n) || true\n";
    }
  };
  scripts.pre = own(scripts.pre, " || exit $?");
  append(scripts.pre, pre);
  scripts.post = own(scripts.post, "");
  append(scripts.post, post);
  return scripts;
}

}  // namespace mira::library
