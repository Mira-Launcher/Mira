#include "library/GameSetup.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <map>
#include <mutex>

#include "core/Paths.h"
#include "library/AutoInstall.h"
#include "library/Detector.h"
#include "library/GamePatch.h"
#include "library/PrefixNaming.h"

namespace mira::library {
namespace {
namespace fs = std::filesystem;
using nlohmann::json;

std::mutex confirmed_mutex;
std::map<fs::path, SetupChoices> confirmed;

// Its prefix, made anew under the name the user gave while there's none on disk yet.
void PlacePrefix(const config::Config& config, const store::GameStore& games, model::Game& game) {
  if (!model::NeedsRunnerSetup(game.platform)) return;
  std::error_code ec;
  if (!game.data_dir.empty() && fs::exists(game.data_dir, ec)) return;
  game.data_dir.clear();
  game.data_dir = PrefixDir(config, games, game).string();
}

}  // namespace

Result<SetupChoices> ParseSetupChoices(const json& body) {
  SetupChoices choices;
  for (const auto& [key, text] :
       {std::pair{"name", &choices.name}, std::pair{"exe_path", &choices.exe_path}}) {
    if (!body.contains(key)) continue;
    if (!body[key].is_string())
      return Err("invalid_body", std::format("\"{}\" must be a string", key));
    *text = body[key].get<std::string>();
  }
  for (const auto& [key, flag] :
       {std::pair{"installer", &choices.installer}, std::pair{"app", &choices.app}}) {
    if (!body.contains(key)) continue;
    if (!body[key].is_boolean())
      return Err("invalid_body", std::format("\"{}\" must be true or false", key));
    *flag = body[key].get<bool>();
  }
  if (choices.name && choices.name->empty()) return Err("invalid_body", "\"name\" can't be empty");
  if (body.contains("steam_appid")) {
    if (!body["steam_appid"].is_number_integer() || body["steam_appid"].get<std::int64_t>() < 0)
      return Err("invalid_body", "\"steam_appid\" must be a number");
    choices.steam_appid = body["steam_appid"].get<std::int64_t>();
  }
  return choices;
}

void ApplyChoices(model::Game& game, const SetupChoices& choices) {
  if (choices.name) game.name = *choices.name;
  if (choices.app) {
    std::erase(game.tags, "app");
    if (*choices.app) game.tags.push_back("app");
  }
  if (choices.steam_appid)
    game.overrides["metadata.steam_appid"] = *choices.steam_appid != 0 ? *choices.steam_appid : -1;
}

Result<void> ConfirmSetup(const config::Config& config, const store::GameStore& games,
                          model::Game& confirmed_game, const SetupChoices& choices) {
  model::Game game = confirmed_game;  // left as it was when the choices don't hold
  // Picked inside its prefix, where an installer put it: that folder is the game's.
  const auto in_prefix = [&](const fs::path& path) {
    return !game.data_dir.empty() && paths::IsWithin(path, {fs::path(game.data_dir)});
  };
  if (choices.exe_path && !choices.installer.value_or(false) && in_prefix(*choices.exe_path) &&
      !(in_prefix(game.install_path) &&
        paths::IsWithin(*choices.exe_path, {fs::path(game.install_path)}))) {
    const fs::path exe = fs::path(*choices.exe_path).lexically_normal();
    AdoptInstallFolder(game, exe.parent_path().string(), exe.filename().string(), game.data_dir);
    game.working_dir.clear();
    game.exe_path = exe.filename().string();
  } else if (choices.exe_path) {
    game.exe_path = StoredExePath(game.install_path, *choices.exe_path);
  }
  std::error_code ec;
  if (game.exe_path.empty() ||
      !fs::is_regular_file(fs::path(game.install_path) / game.exe_path, ec))
    return Err("no_executable",
               std::format("there's no program at {}",
                           (fs::path(game.install_path) / game.exe_path).string()),
               "Choose the program that starts it.");

  ApplyChoices(game, choices);
  game.platform = PlatformOfFile(game.exe_path);
  const auto found = std::ranges::find(game.candidates, game.exe_path, &model::Candidate::rel_path);
  const bool installer = choices.installer.value_or(
      found != game.candidates.end()
          ? found->is_installer
          : LooksLikeInstaller(fs::path(game.exe_path).filename(), SettingsFromConfig(config)));
  game.candidates = {{.rel_path = game.exe_path,
                      .kind = game.platform,
                      .score = 1.0,
                      .chosen = true,
                      .is_installer = installer}};
  game.reviewed = true;
  game.last_error.clear();
  // Set first: an installer's prefix goes elsewhere than a game's.
  game.setup = installer ? model::SetupStep::Install : model::SetupStep::None;
  PlacePrefix(config, games, game);
  game.status = installer ? model::GameStatus::NotSetUp : RunnableStatus(game);
  confirmed_game = std::move(game);
  return {};
}

void ConfirmWhenFound(const fs::path& folder, SetupChoices choices) {
  const std::lock_guard lock(confirmed_mutex);
  confirmed[folder.lexically_normal()] = std::move(choices);
}

std::optional<SetupChoices> TakeConfirmed(const fs::path& folder) {
  const std::lock_guard lock(confirmed_mutex);
  auto found = confirmed.extract(folder.lexically_normal());
  if (found.empty()) return std::nullopt;
  return std::move(found.mapped());
}

model::GameStatus RunnableStatus(const model::Game& game) {
  if (game.exe_path.empty()) return model::GameStatus::SettingUp;
  if (!model::NeedsRunnerSetup(game.platform)) return model::GameStatus::Ready;
  // Only ready if something actually provisioned it.
  std::error_code ec;
  const bool provisioned =
      !game.runner_ref.empty() && !game.data_dir.empty() && fs::exists(game.data_dir, ec);
  return provisioned ? model::GameStatus::Ready : model::GameStatus::SettingUp;
}

}  // namespace mira::library
