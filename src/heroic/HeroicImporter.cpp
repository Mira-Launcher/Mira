#include "heroic/HeroicImporter.h"

#include <algorithm>
#include <fstream>
#include <json.hpp>
#include <map>
#include <string>
#include <vector>

#include "core/Json.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "library/Detector.h"
#include "library/Relocate.h"

namespace mira::heroic {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

// One installed game, as Heroic's files describe it.
struct Installed {
  std::string store;  // "epic", "gog", "amazon" or "sideload"
  std::string app;    // Heroic's appName
  std::string title;
  fs::path exe = {};  // absolute
  std::string args = {};
  bool native = false;
};

// A file Heroic wrote, or a discarded json when it's missing or broken.
json ReadJson(const fs::path& file) {
  std::ifstream in(file);
  if (!in) return json(json::value_t::discarded);
  return json::parse(in, nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
}

// legendaryConfig/legendary/installed.json: {"<app>": {title, install_path, executable, platform,
// is_dlc, launch_parameters}}.
void ReadEpic(const fs::path& dir, std::vector<Installed>& out, HeroicImportSummary& summary) {
  const json installed = ReadJson(dir / "legendaryConfig" / "legendary" / "installed.json");
  if (!installed.is_object()) return;
  for (const auto& [app, entry] : installed.items()) {
    if (core::JsonBool(entry, "is_dlc")) continue;
    const std::string platform = core::JsonString(entry, "platform");
    if (platform == "Mac") {
      ++summary.unsupported;
      continue;
    }
    const std::string executable = core::JsonString(entry, "executable");
    const std::string install_path = core::JsonString(entry, "install_path");
    if (executable.empty() || install_path.empty()) {
      ++summary.incomplete;
      continue;
    }
    out.push_back({.store = "epic",
                   .app = app,
                   .title = core::JsonString(entry, "title", app),
                   .exe = fs::path(install_path) / executable,
                   .args = core::JsonString(entry, "launch_parameters"),
                   .native = platform == "Linux"});
  }
}

// gog_store/installed.json: {"installed": [{appName, install_path, platform, is_dlc}]}, the titles
// in gog_store/library.json, and the program in the game's goggame-<app>.info (start.sh on Linux).
void ReadGog(const fs::path& dir, std::vector<Installed>& out, HeroicImportSummary& summary) {
  const json installed = ReadJson(dir / "gog_store" / "installed.json");
  if (!installed.is_object() || !installed.contains("installed")) return;
  std::map<std::string, std::string> titles;
  const json library = ReadJson(dir / "gog_store" / "library.json");
  if (library.is_object() && library.contains("games") && library["games"].is_array()) {
    for (const json& game : library["games"])
      titles[core::JsonString(game, "app_name")] = core::JsonString(game, "title");
  }
  for (const json& entry : installed["installed"]) {
    if (core::JsonBool(entry, "is_dlc")) continue;
    const std::string app = core::JsonString(entry, "appName");
    const fs::path install_path = core::JsonString(entry, "install_path");
    const std::string platform = core::JsonString(entry, "platform");
    if (platform == "osx") {
      ++summary.unsupported;
      continue;
    }
    if (install_path.empty()) {
      ++summary.incomplete;
      continue;
    }
    Installed game{.store = "gog",
                   .app = app,
                   .title = titles[app].empty() ? install_path.filename().string() : titles[app],
                   .native = platform == "linux"};
    if (game.native) {
      game.exe = install_path / "start.sh";
    } else {
      const json info = ReadJson(install_path / ("goggame-" + app + ".info"));
      if (info.is_object() && info.contains("playTasks") && info["playTasks"].is_array()) {
        for (const json& task : info["playTasks"]) {
          if (!core::JsonBool(task, "isPrimary") || core::JsonString(task, "path").empty())
            continue;
          game.exe = install_path / core::JsonString(task, "path");
          game.args = core::JsonString(task, "arguments");
          break;
        }
      }
    }
    if (app.empty() || game.exe.empty()) {
      ++summary.incomplete;
      continue;
    }
    out.push_back(std::move(game));
  }
}

// nile_config/nile/installed.json: [{id, path}], the titles in library.json and the program in the
// game's fuel.json. Amazon's games are all Windows ones.
void ReadAmazon(const fs::path& dir, std::vector<Installed>& out, HeroicImportSummary& summary) {
  const fs::path nile = dir / "nile_config" / "nile";
  const json installed = ReadJson(nile / "installed.json");
  if (!installed.is_array()) return;
  std::map<std::string, std::string> titles;
  const json library = ReadJson(nile / "library.json");
  for (const json& game : library.is_array() ? library : json::array()) {
    if (game.is_object() && game.contains("product"))
      titles[core::JsonString(game, "id")] = core::JsonString(game["product"], "title");
  }
  for (const json& entry : installed) {
    const std::string app = core::JsonString(entry, "id");
    const fs::path install_path = core::JsonString(entry, "path");
    if (install_path.empty()) {
      ++summary.incomplete;
      continue;
    }
    const json fuel = ReadJson(install_path / "fuel.json");
    const json main = fuel.is_object() ? fuel.value("Main", json::object()) : json::object();
    const std::string command = core::JsonString(main, "Command");
    if (app.empty() || command.empty()) {
      ++summary.incomplete;
      continue;
    }
    std::vector<std::string> args;
    if (main.contains("Args") && main["Args"].is_array()) {
      for (const json& arg : main["Args"])
        if (arg.is_string()) args.push_back(arg.get<std::string>());
    }
    out.push_back({.store = "amazon",
                   .app = app,
                   .title = titles[app].empty() ? install_path.filename().string() : titles[app],
                   .exe = install_path / command,
                   .args = strings::JoinArgs(args),
                   .native = false});
  }
}

// sideload_apps/library.json: {"games": [{app_name, title, install: {executable, platform}}]}.
void ReadSideloaded(const fs::path& dir, std::vector<Installed>& out,
                    HeroicImportSummary& summary) {
  const json library = ReadJson(dir / "sideload_apps" / "library.json");
  if (!library.is_object() || !library.contains("games") || !library["games"].is_array()) return;
  for (const json& entry : library["games"]) {
    const json install = entry.value("install", json::object());
    const std::string platform = strings::ToLower(core::JsonString(install, "platform"));
    const fs::path exe = core::JsonString(install, "executable");
    if (platform != "windows" && platform != "linux" && platform != "native") {
      ++summary.unsupported;  // a browser game, or a macOS one
      continue;
    }
    if (core::JsonString(entry, "app_name").empty() || !exe.is_absolute()) {
      ++summary.incomplete;
      continue;
    }
    out.push_back({.store = "sideload",
                   .app = core::JsonString(entry, "app_name"),
                   .title = core::JsonString(entry, "title", exe.stem().string()),
                   .exe = exe,
                   .args = {},
                   .native = platform != "windows"});
  }
}

// The game's Wine prefix from GamesConfig/<app>.json. A Proton prefix keeps it in pfx/.
std::optional<fs::path> PrefixOf(const fs::path& dir, const std::string& app) {
  const json config = ReadJson(dir / "GamesConfig" / (app + ".json"));
  if (!config.is_object() || !config.contains(app)) return std::nullopt;
  const fs::path prefix = core::JsonString(config[app], "winePrefix");
  if (prefix.empty()) return std::nullopt;
  std::error_code ec;
  if (fs::exists(prefix / "pfx" / "drive_c", ec)) return prefix / "pfx";
  return prefix;
}

}  // namespace

std::optional<fs::path> FindHeroicConfigDir(const config::Config& config) {
  // A folder set is the only place looked: Heroic isn't somewhere else as well.
  std::vector<fs::path> candidates;
  if (const fs::path configured = config.GetPath("heroic.config_dir"); !configured.empty()) {
    candidates.push_back(configured);
  } else {
    candidates.push_back(paths::EnvOr("XDG_CONFIG_HOME", paths::Home() / ".config") / "heroic");
    candidates.push_back(paths::Home() / ".var" / "app" / "com.heroicgameslauncher.hgl" / "config" /
                         "heroic");
  }
  std::error_code ec;
  for (const fs::path& candidate : candidates) {
    if (fs::exists(candidate / "config.json", ec) || fs::exists(candidate / "GamesConfig", ec))
      return candidate;
  }
  return std::nullopt;
}

HeroicImporter::HeroicImporter(config::Config& config, store::GameStore& games,
                               api::EventBus& events)
    : config_(config), games_(games), events_(events) {}

Result<HeroicImportSummary> HeroicImporter::Import() {
  HeroicImportSummary summary;
  if (!games_.SourceEnabled("heroic")) return summary;
  const auto dir = FindHeroicConfigDir(config_);
  if (!dir) {
    return Err("heroic_not_found", "Mira couldn't find Heroic Games Launcher's settings",
               "If Heroic keeps them somewhere unusual, set its config folder.",
               Fix::Setting("heroic.config_dir"));
  }

  std::vector<Installed> installed;
  ReadEpic(*dir, installed, summary);
  ReadGog(*dir, installed, summary);
  ReadAmazon(*dir, installed, summary);
  ReadSideloaded(*dir, installed, summary);

  std::vector<model::Game> library = games_.All();
  const auto keep = [&](const model::Game& saved) {
    std::erase_if(library, [&](const model::Game& known) { return known.id == saved.id; });
    library.push_back(saved);
  };
  for (const Installed& found : installed) {
    std::string prefix;
    if (!found.native) {
      const auto prefix_dir = PrefixOf(*dir, found.app);
      if (!prefix_dir) {
        ++summary.incomplete;
        continue;
      }
      prefix = prefix_dir->string();
    }
    const std::string ref = found.store + ":" + found.app;
    const std::string install_path = found.exe.parent_path().string();
    const std::string exe_path = found.exe.filename().string();

    // By Heroic's id first; by folder for a game Mira found on its own. A game Mira installed from
    // its own store source at that folder is that store's, not Heroic's.
    std::optional<model::Game> existing;
    for (const model::Game& known : library) {
      if (known.source == "heroic" && known.source_ref == ref) existing = known;
    }
    if (!existing) {
      auto at_path = games_.FindByInstallPath(install_path);
      if (at_path && at_path->source != "manual" && at_path->source != "scan" &&
          at_path->source != "heroic")
        continue;
      if (at_path && (at_path->source != "heroic" || at_path->source_ref.empty()))
        existing = std::move(at_path);
    }

    // Heroic sets where the game is when it arrives; after that the program and folders are
    // Mira's to change, and Heroic's only come back if Mira's are gone.
    std::error_code ec;
    model::Game game = existing.value_or(model::Game{});
    game.id = existing ? game.id : games_.NextId(found.title, install_path);
    game.source = "heroic";
    game.source_ref = ref;
    game.name = found.title;
    const bool own_program =
        existing && fs::exists(fs::path(existing->install_path) / existing->exe_path, ec);
    if (!own_program) {
      game.install_path = install_path;
      game.exe_path = exe_path;
    }
    if (!existing || game.args == core::JsonString(existing->runner_config, "heroic_args"))
      game.args = found.args;
    game.runner_config["heroic_args"] = found.args;
    if (!existing || existing->data_dir.empty() || !fs::exists(existing->data_dir, ec))
      game.data_dir = prefix;
    if (game.candidates.empty() && library::SharedFolder(config_, game, library).empty()) {
      game.candidates = library::Detector(library::SettingsFromConfig(config_))
                            .Detect(game.install_path)
                            .candidates;
      for (model::Candidate& candidate : game.candidates)
        candidate.chosen = candidate.rel_path == game.exe_path;
      if (std::ranges::none_of(game.candidates, &model::Candidate::chosen)) {
        game.candidates.insert(
            game.candidates.begin(),
            {.rel_path = game.exe_path,
             .kind = found.native ? model::Platform::Native : model::Platform::Windows,
             .score = 1.0,
             .chosen = true,
             .is_installer = false});
      }
    }
    game.platform = found.native ? model::Platform::Native : model::Platform::Windows;
    // Heroic's runner names ("Proton - GE-Proton9-1") aren't Mira's references, so the game uses
    // default_runner.windows unless one is picked in Mira.
    game.status = model::GameStatus::Ready;
    game.last_error.clear();
    game.updated_at = model::NowSeconds();
    if (!existing) game.created_at = game.updated_at;

    auto result = games_.Merge(existing, game);
    if (!result) {
      log::Error("failed to save heroic game {}: {}", game.id, result.error().message);
      continue;
    }
    keep(game);
    library::RecordImported(summary, events_, game, existing.has_value());
  }
  return summary;
}

}  // namespace mira::heroic
