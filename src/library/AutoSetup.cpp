#include "library/AutoSetup.h"

#include "core/Log.h"
#include "core/Strings.h"
#include "library/AutoInstall.h"
#include "library/FolderTags.h"
#include "library/GameSetup.h"
#include "library/PrefixNaming.h"

namespace mira::library {
namespace {
namespace fs = std::filesystem;

// A new game's tags from where it was found in `folder`: the folder tag of a sorting folder
// first, `app` in the Applications root (whatever lands there is an application, not a game), and
// `hidden` under .hidden. Outside the library folders (an install inside a prefix) none apply.
void TagFromPlace(const config::Config& config, model::Game& game, const fs::path& folder) {
  const fs::path root = RootOf(config, folder);
  if (root.empty()) return;
  const Container place = ContainerOf(config, root, folder).value_or(Container{});
  if (!place.folder_tag.empty()) game.tags.push_back(place.folder_tag);
  if (strings::ToLower(root.filename().string()) == "applications") game.tags.push_back("app");
  if (place.hidden) game.tags.push_back("hidden");
}
}  // namespace

AutoSetup::AutoSetup(config::Config& config, store::GameStore& games, api::EventBus& events)
    : config_(config), games_(games), events_(events) {}

// Answered for before it was found (an archive's card): nothing left to ask.
void AutoSetup::ConfirmIfAnswered(const fs::path& found, model::Game& game) const {
  const auto choices = TakeConfirmed(found);
  if (!choices) return;
  // Its program only showed once unpacked (a native one without an extension): that's still asked.
  if (!choices->exe_path) return ApplyChoices(game, *choices);
  if (auto set = ConfirmSetup(config_, games_, game, *choices); !set)
    log::Warn("{} waits to be set up: {}", found.string(), set.error().message);
}

model::Game AutoSetup::CreateGame(const fs::path& install_path, const Detector::Result& detected) {
  model::Game game;
  game.name = strings::CleanGameName(install_path.filename().string());
  game.id = games_.NextId(game.name, install_path.string());
  game.install_path = install_path.string();
  game.confidence = detected.confidence;
  game.candidates = detected.candidates;
  game.created_at = model::NowSeconds();
  game.updated_at = game.created_at;
  TagFromPlace(config_, game, install_path.parent_path());

  if (detected.candidates.empty()) {
    game.status = model::GameStatus::NotSetUp;
    game.setup = model::SetupStep::Failed;
    game.platform = model::Platform::Unknown;
    game.last_error = "No program found in this folder. Choose the one that starts it.";
  } else {
    const model::Candidate& top = detected.candidates.front();
    game.platform = top.kind;
    game.exe_path = top.rel_path;
    game.status = model::GameStatus::NotSetUp;
    game.setup = top.is_installer ? model::SetupStep::Install : model::SetupStep::Confirm;
  }
  // After its step: an installer's prefix never goes in its folder.
  game.data_dir = PrefixDir(config_, games_, game).string();
  ConfirmIfAnswered(install_path, game);

  if (auto result = games_.Upsert(game); !result) {
    log::Error("failed to save new game \"{}\": {}", game.id, result.error().message);
    return game;
  }

  nlohmann::json payload = model::ToJson(game);
  payload["auto_install"] = AutoInstalls(config_, game);
  events_.Publish("game.added", std::move(payload));

  return game;
}

model::Game AutoSetup::CreateAppImageGame(const fs::path& root, const fs::path& appimage) {
  model::Game game;
  game.name = strings::CleanGameName(appimage.stem().string());
  game.id = games_.NextId(game.name, root.string(), appimage.filename().string());
  game.install_path = root.string();
  game.exe_path = appimage.filename().string();
  game.platform = model::Platform::Native;
  game.status = model::GameStatus::NotSetUp;
  game.setup = model::SetupStep::Confirm;
  game.confidence = 1.0;
  game.candidates = {{.rel_path = game.exe_path,
                      .kind = model::Platform::Native,
                      .score = 1.0,
                      .chosen = true,
                      .is_installer = false}};
  game.created_at = model::NowSeconds();
  game.updated_at = game.created_at;
  TagFromPlace(config_, game, root);
  ConfirmIfAnswered(appimage, game);

  if (auto result = games_.Upsert(game); !result) {
    log::Error("failed to save new game \"{}\": {}", game.id, result.error().message);
    return game;
  }
  nlohmann::json payload = model::ToJson(game);
  payload["auto_install"] = false;
  events_.Publish("game.added", std::move(payload));
  return game;
}

}  // namespace mira::library
