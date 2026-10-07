#include "library/AutoSetup.h"

#include "core/Log.h"
#include "core/Strings.h"
#include "library/AutoInstall.h"
#include "library/FolderTags.h"
#include "library/PrefixNaming.h"

namespace mira::library {
namespace {
namespace fs = std::filesystem;

// A new game's tags from where it was found in `folder`: the folder tag of a sorting folder
// first, the library root's name (scan.tag_by_root), `app` in the Applications root (whatever
// lands there is an application, not a game), and `hidden` under .hidden. Outside the library
// folders (an install inside a prefix) none apply.
void TagFromPlace(const config::Config& config, model::Game& game, const fs::path& folder) {
  const fs::path root = RootOf(config, folder);
  if (root.empty()) return;
  const Container place = ContainerOf(config, root, folder).value_or(Container{});
  if (!place.folder_tag.empty()) game.tags.push_back(place.folder_tag);
  if (config.GetBool("scan.tag_by_root") && !root.filename().empty())
    game.tags.push_back(root.filename().string());
  if (strings::ToLower(root.filename().string()) == "applications") game.tags.push_back("app");
  if (place.hidden) game.tags.push_back("hidden");
}
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
  game.data_dir = PrefixDir(config_, games_, game).string();

  TagFromPlace(config_, game, install_path.parent_path());

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

model::Game AutoSetup::CreateAppImageGame(const fs::path& root, const fs::path& appimage) {
  model::Game game;
  game.name = strings::CleanGameName(appimage.stem().string());
  game.id = games_.NextId(game.name);
  game.install_path = root.string();
  game.exe_path = appimage.filename().string();
  game.platform = model::Platform::Native;
  game.status = model::GameStatus::Ready;  // an AppImage carries everything it needs
  game.confidence = 1.0;
  game.candidates = {{.rel_path = game.exe_path, .kind = model::Platform::Native, .score = 1.0, .chosen = true,
                      .is_installer = false}};
  game.created_at = model::NowSeconds();
  game.updated_at = game.created_at;
  TagFromPlace(config_, game, root);

  if (auto result = games_.Upsert(game); !result) {
    log::Error("failed to save new game \"{}\": {}", game.id, result.error().message);
    return game;
  }
  nlohmann::json payload = model::ToJson(game);
  payload["open_config"] = config_.GetBool("open_config_on_add");
  payload["auto_install"] = false;
  events_.Publish("game.added", std::move(payload));
  return game;
}

}  // namespace mira::library
