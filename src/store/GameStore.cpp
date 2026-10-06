#include "store/GameStore.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <set>

#include <toml.hpp>

#include "core/AtomicFile.h"
#include "core/Log.h"
#include "core/Strings.h"
#include "core/TomlJson.h"

namespace mira::store {
namespace {
using nlohmann::json;
}

GameStore::GameStore(std::filesystem::path file) : file_(std::move(file)) {}

void GameStore::Load() {
  std::lock_guard lock(mutex_);
  games_.clear();

  std::error_code ec;
  if (!std::filesystem::exists(file_, ec)) return;  // no games yet; not an error

  toml::parse_result parsed = toml::parse_file(file_.string());
  if (!parsed) {
    const auto broken = file_.string() + ".bad";
    std::filesystem::rename(file_, broken, ec);
    log::Error("games at {} could not be parsed ({}); kept it as {} and starting with an "
              "empty library",
              file_.string(), parsed.error().description(), broken);
    return;
  }

  const json whole = tomljson::ToJson(parsed.table());
  if (!whole.contains("game") || !whole["game"].is_array()) return;

  for (const json& entry : whole["game"]) {
    model::Game game = model::GameFromJson(entry);
    if (game.id.empty()) {
      log::Warn("skipping a game entry in {} with no id", file_.string());
      continue;
    }
    games_.push_back(std::move(game));
  }
  log::Info("loaded {} game(s) from {}", games_.size(), file_.string());
}

Result<void> GameStore::Save() {
  // Every caller (Upsert/Update/Remove) releases mutex_ before calling this,
  // so locking again here is safe, not a re-entrant deadlock -- and
  // necessary: without it, this read of games_ raced a concurrent Update()
  // on another thread (TSan caught this for real, not hypothetically, once
  // ProcessSupervisor's own background watcher thread and a caller thread
  // both touched the same GameStore around the same time). Copied under the
  // lock, then serialized/written from the copy so a slow disk write never
  // holds mutex_ and blocks an unrelated Find()/Update() the whole time.
  {
    std::lock_guard lock(mutex_);
    if (batch_depth_ > 0) {
      batch_dirty_ = true;
      return {};
    }
  }
  std::lock_guard save_lock(save_mutex_);
  std::vector<model::Game> games_copy;
  {
    std::lock_guard lock(mutex_);
    games_copy = games_;
  }

  json whole = json::object();
  whole["game"] = json::array();
  for (const model::Game& game : games_copy) whole["game"].push_back(model::ToJson(game));

  return WriteFileAtomic(file_, tomljson::ToTomlText(whole), "games_write_failed");
}

GameStore::SaveBatch::SaveBatch(GameStore& store) : store_(store) {
  std::lock_guard lock(store_.mutex_);
  ++store_.batch_depth_;
}

GameStore::SaveBatch::~SaveBatch() {
  bool write = false;
  {
    std::lock_guard lock(store_.mutex_);
    write = --store_.batch_depth_ == 0 && store_.batch_dirty_;
    if (write) store_.batch_dirty_ = false;
  }
  if (!write) return;
  if (auto saved = store_.Save(); !saved) log::Error("could not save games: {}", saved.error().message);
}

std::vector<model::Game> GameStore::All() const {
  std::lock_guard lock(mutex_);
  return games_;
}

std::optional<model::Game> GameStore::Find(const std::string& id) const {
  std::lock_guard lock(mutex_);
  const auto it = std::ranges::find(games_, id, &model::Game::id);
  if (it == games_.end()) return std::nullopt;
  return *it;
}

std::optional<model::Game> GameStore::FindByInstallPath(const std::string& install_path) const {
  std::lock_guard lock(mutex_);
  const auto it = std::ranges::find(games_, install_path, &model::Game::install_path);
  if (it == games_.end()) return std::nullopt;
  return *it;
}

bool GameStore::HasInstallUnder(const std::string& dir) const {
  std::lock_guard lock(mutex_);
  return std::ranges::any_of(games_, [&dir](const model::Game& game) {
    return game.install_path.size() > dir.size() && game.install_path.starts_with(dir) &&
           game.install_path[dir.size()] == '/';
  });
}

std::string GameStore::NextId(const std::string& name) const {
  const std::string base = strings::Slugify(name);
  std::lock_guard lock(mutex_);
  if (std::ranges::none_of(games_, [&](const model::Game& g) { return g.id == base; })) {
    return base;
  }
  for (int suffix = 2;; ++suffix) {
    const std::string candidate = std::format("{}-{}", base, suffix);
    if (std::ranges::none_of(games_, [&](const model::Game& g) { return g.id == candidate; })) {
      return candidate;
    }
  }
}

Result<void> GameStore::Upsert(model::Game game) {
  {
    std::lock_guard lock(mutex_);
    auto it = std::ranges::find(games_, game.id, &model::Game::id);
    if (it == games_.end()) {
      games_.push_back(std::move(game));
    } else {
      *it = std::move(game);
    }
  }
  return Save();
}

Result<void> GameStore::Merge(const std::optional<model::Game>& base, model::Game& game) {
  {
    std::lock_guard lock(mutex_);
    auto it = std::ranges::find(games_, game.id, &model::Game::id);
    if (it == games_.end() || !base) {
      if (it == games_.end()) {
        games_.push_back(game);
      } else {
        *it = game;
      }
    } else {
      const nlohmann::json before = model::ToJson(*base);
      const nlohmann::json edited = model::ToJson(game);
      nlohmann::json live = model::ToJson(*it);
      for (const auto& [key, value] : edited.items()) {
        if (key != "tags" && before.value(key, nlohmann::json()) != value) live[key] = value;
      }
      model::Game merged = model::GameFromJson(live);
      for (const std::string& tag : game.tags) {
        if (std::ranges::find(base->tags, tag) == base->tags.end() && std::ranges::find(merged.tags, tag) == merged.tags.end()) {
          merged.tags.push_back(tag);
        }
      }
      std::erase_if(merged.tags, [&](const std::string& tag) {
        return std::ranges::find(base->tags, tag) != base->tags.end() && std::ranges::find(game.tags, tag) == game.tags.end();
      });
      *it = std::move(merged);
    }
    game = *std::ranges::find(games_, game.id, &model::Game::id);
  }
  return Save();
}

Result<model::Game> GameStore::Update(const std::string& id,
                                       std::function<void(model::Game&)> mutator) {
  model::Game updated;
  {
    std::lock_guard lock(mutex_);
    auto it = std::ranges::find(games_, id, &model::Game::id);
    if (it == games_.end()) return Err("game_not_found", std::format("no game with id \"{}\"", id));
    mutator(*it);
    it->updated_at = model::NowSeconds();
    updated = *it;
  }
  if (auto result = Save(); !result) return std::unexpected(result.error());
  return updated;
}

Result<std::vector<model::Game>> GameStore::UpdateMany(const std::vector<std::string>& ids,
                                                       std::function<bool(model::Game&)> mutator) {
  std::vector<model::Game> updated;
  {
    std::lock_guard lock(mutex_);
    const std::set<std::string> wanted(ids.begin(), ids.end());
    for (model::Game& game : games_) {
      if (!wanted.contains(game.id) || !mutator(game)) continue;
      game.updated_at = model::NowSeconds();
      updated.push_back(game);
    }
  }
  if (updated.empty()) return updated;
  if (auto result = Save(); !result) return std::unexpected(result.error());
  return updated;
}

Result<std::vector<std::string>> GameStore::RemoveMany(const std::vector<std::string>& ids) {
  std::vector<std::string> removed;
  {
    std::lock_guard lock(mutex_);
    const std::set<std::string> wanted(ids.begin(), ids.end());
    std::erase_if(games_, [&](const model::Game& game) {
      if (!wanted.contains(game.id)) return false;
      removed.push_back(game.id);
      return true;
    });
  }
  if (removed.empty()) return removed;
  if (auto result = Save(); !result) return std::unexpected(result.error());
  return removed;
}

Result<void> GameStore::Remove(const std::string& id) {
  {
    std::lock_guard lock(mutex_);
    const auto [first, last] = std::ranges::remove(games_, id, &model::Game::id);
    if (first == games_.end()) {
      return Err("game_not_found", std::format("no game with id \"{}\"", id));
    }
    games_.erase(first, last);
  }
  return Save();
}

}  // namespace mira::store
