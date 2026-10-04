#include "library/AutoSetup.h"

#include "core/Log.h"
#include "core/Strings.h"
#include "library/AutoInstall.h"
#include "library/PrefixNaming.h"

namespace mira::library {
namespace {
namespace fs = std::filesystem;
}

AutoSetup::AutoSetup(config::Config& config, store::GameStore& games, api::EventBus& events)
    : config_(config), games_(games), events_(events) {}

model::Game AutoSetup::CreateGame(const fs::path& install_path, const Detector::Result& detected) {
  model::Game game;
  game.name = strings::CleanGameName(install_path.filename().string());
  game.id = games_.NextId(game.name);
  game.install_path = install_path.string();
  game.confidence = detected.confidence;
  game.candidates = detected.candidates;
  game.created_at = model::NowSeconds();
  game.updated_at = game.created_at;
  game.data_dir = PrefixDir(config_, game).string();

  // install_path's parent is which library root this came from (see the
  // field's own comment in model/Types.h) -- the root's own leaf folder
  // name doubles as a natural, human-readable tag for it, letting multiple
  // library_roots stay filterable (GET /v1/games?tag=...) without the user
  // tagging anything by hand.
  if (config_.GetBool("scan.tag_by_root")) {
    const std::string root_name = install_path.parent_path().filename().string();
    if (!root_name.empty()) game.tags.push_back(root_name);
  }

  if (detected.candidates.empty()) {
    game.status = model::GameStatus::Broken;
    game.platform = model::Platform::Unknown;
    game.last_error = "No executable found automatically. Set one manually.";
  } else {
    const model::Candidate& top = detected.candidates.front();
    game.platform = top.kind;
    game.exe_path = top.rel_path;
    if (top.is_installer) {
      // Running an installer isn't running the game: flag it rather than
      // silently provisioning/launching a setup wizard as if it were.
      game.status = model::GameStatus::NeedsInstall;
      game.last_error = "This looks like an installer (" + top.rel_path + "), not the game itself. Install it to play.";
    } else if (top.kind == model::Platform::Native) {
      game.status = model::GameStatus::Ready;  // native needs no provisioning
    } else {
      game.status = model::GameStatus::SettingUp;  // waits for the runner layer to provision it
    }
  }

  if (auto result = games_.Upsert(game); !result) {
    log::Error("failed to save new game \"{}\": {}", game.id, result.error().message);
    return game;
  }

  nlohmann::json payload = model::ToJson(game);
  payload["open_config"] = config_.GetBool("open_config_on_add");
  payload["auto_install"] = AutoInstalls(config_, game);
  events_.Publish("game.added", std::move(payload));

  return game;
}

}  // namespace mira::library
