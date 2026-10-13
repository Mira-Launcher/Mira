#include "library/LaunchOptions.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <set>

#include "core/Strings.h"

namespace mira::library {
using nlohmann::json;
namespace {

constexpr std::pair<LaunchMode, std::string_view> kModes[] = {
    {LaunchMode::Play, "play"}, {LaunchMode::Alongside, "alongside"}, {LaunchMode::Tool, "tool"}};

std::optional<LaunchMode> ModeFromString(std::string_view text) {
  for (const auto& [mode, name] : kModes) {
    if (name == text) return mode;
  }
  return std::nullopt;
}

std::string WinePath(const std::string& path) {
  std::string out = "Z:" + path;
  std::ranges::replace(out, '/', '\\');
  return out;
}

}  // namespace

std::vector<LaunchOption> LaunchOptions(const model::Game& game) {
  std::vector<LaunchOption> options;
  const auto list = game.launch.find("options");
  if (list == game.launch.end() || !list->is_array()) return options;
  for (const json& entry : *list) {
    if (!entry.is_object()) continue;
    LaunchOption option;
    option.id = entry.value("id", std::string());
    option.name = entry.value("name", std::string());
    option.mode = ModeFromString(entry.value("mode", std::string("play"))).value_or(LaunchMode::Play);
    option.exe = entry.value("exe", std::string());
    option.args = entry.value("args", std::string());
    option.native = entry.value("native", false);
    if (!option.id.empty() && !option.exe.empty()) options.push_back(std::move(option));
  }
  return options;
}

std::optional<LaunchOption> FindLaunchOption(const model::Game& game, std::string_view id) {
  for (LaunchOption& option : LaunchOptions(game)) {
    if (option.id == id) return std::move(option);
  }
  return std::nullopt;
}

std::optional<LaunchOption> DefaultLaunch(const model::Game& game) {
  const std::string id = game.launch.value("default", std::string());
  if (id.empty()) return std::nullopt;
  auto option = FindLaunchOption(game, id);
  if (option && option->mode != LaunchMode::Play) return std::nullopt;
  return option;
}

std::optional<std::string> LaunchProblem(const json& launch) {
  if (!launch.is_object()) return "\"launch\" must be an object";
  if (launch.contains("default") && !launch["default"].is_string()) return "\"launch.default\" must be a string";
  if (launch.contains("only") && !launch["only"].is_boolean()) return "\"launch.only\" must be true or false";
  if (!launch.contains("options")) return std::nullopt;
  if (!launch["options"].is_array()) return "\"launch.options\" must be an array";
  std::set<std::string> ids;
  for (const json& option : launch["options"]) {
    if (!option.is_object()) return "each launch option must be an object";
    for (const char* key : {"id", "name", "mode", "exe", "args"}) {
      if (option.contains(key) && !option[key].is_string()) return std::format("a launch option's \"{}\" must be a string", key);
    }
    if (option.contains("native") && !option["native"].is_boolean()) return "a launch option's \"native\" must be true or false";
    if (option.value("name", std::string()).empty()) return "each launch option needs a name";
    if (option.value("exe", std::string()).empty()) return "each launch option needs an exe";
    if (!ModeFromString(option.value("mode", std::string("play")))) {
      return "a launch option's \"mode\" must be \"play\", \"alongside\" or \"tool\"";
    }
    const std::string id = option.value("id", std::string());
    if (id == kGameLaunch) return std::format("\"{}\" names the game's own launch", kGameLaunch);
    if (!id.empty() && !ids.insert(id).second) return std::format("two launch options are \"{}\"", id);
  }
  return std::nullopt;
}

json WithLaunchIds(json launch) {
  if (!launch.contains("options") || !launch["options"].is_array()) return launch;
  std::set<std::string> taken = {std::string(kGameLaunch)};
  for (const json& option : launch["options"]) taken.insert(option.value("id", std::string()));
  for (json& option : launch["options"]) {
    if (!option.value("id", std::string()).empty()) continue;
    const std::string base = strings::Slugify(option.value("name", std::string()));
    std::string id = base;
    for (int n = 2; taken.contains(id); ++n) id = std::format("{}-{}", base, n);
    taken.insert(id);
    option["id"] = id;
  }
  return launch;
}

std::string FillLaunchPlaceholders(std::string_view text, const model::Game& game, bool wine_paths) {
  const auto path = [&](const std::string& value) { return wine_paths && !value.empty() ? WinePath(value) : value; };
  const std::string exe = game.exe_path.empty() ? std::string()
                                                : (std::filesystem::path(game.install_path) / game.exe_path).string();
  std::string out(text);
  for (const auto& [name, value] : {std::pair<std::string_view, std::string>{"{game_dir}", path(game.install_path)},
                                    {"{game_exe}", path(exe)},
                                    {"{prefix}", path(game.data_dir)}}) {
    for (size_t at = out.find(name); at != std::string::npos; at = out.find(name, at + value.size())) {
      out.replace(at, name.size(), value);
    }
  }
  return out;
}

}  // namespace mira::library
