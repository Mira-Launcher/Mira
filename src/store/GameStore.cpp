#include "store/GameStore.h"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <set>

#include <toml.hpp>

#include "core/AtomicFile.h"
#include "core/Log.h"
#include "core/Strings.h"
#include "core/TomlJson.h"

namespace mira::store {
namespace {
using nlohmann::json;
namespace fs = std::filesystem;

// Each step runs once, in order; append, never edit one that has shipped.
constexpr std::array<std::string_view, 1> kMigrations = {
    R"sql(
CREATE TABLE games(
  id TEXT PRIMARY KEY,
  install_path TEXT NOT NULL,
  name TEXT NOT NULL,
  status TEXT NOT NULL CHECK(status IN ('setting_up', 'ready', 'broken', 'missing', 'needs_install')),
  confidence REAL NOT NULL,
  reviewed INTEGER NOT NULL,
  platform TEXT NOT NULL,
  source TEXT NOT NULL,
  exe_path TEXT NOT NULL,
  args TEXT NOT NULL,
  working_dir TEXT NOT NULL,
  runner_ref TEXT NOT NULL,
  source_ref TEXT NOT NULL,
  data_dir TEXT NOT NULL,
  installer_dir TEXT NOT NULL,
  last_error TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  last_played_at INTEGER,
  play_seconds INTEGER NOT NULL,
  last_session_at INTEGER NOT NULL,
  runner_config TEXT NOT NULL CHECK(json_valid(runner_config)),
  overrides TEXT NOT NULL CHECK(json_valid(overrides)),
  env TEXT NOT NULL CHECK(json_valid(env)),
  candidates TEXT NOT NULL CHECK(json_valid(candidates))
) STRICT;
CREATE INDEX games_install_path ON games(install_path);
CREATE INDEX games_data_dir ON games(data_dir);
CREATE TABLE game_tags(
  game_id TEXT NOT NULL REFERENCES games ON DELETE CASCADE,
  position INTEGER NOT NULL,
  tag TEXT NOT NULL,
  PRIMARY KEY(game_id, tag)
) STRICT;
-- mira-run writes a record as a session starts and ends; mirad counts it.
CREATE TABLE sessions(
  game_id TEXT NOT NULL REFERENCES games ON DELETE CASCADE,
  started_at INTEGER NOT NULL,
  wrapper_pid INTEGER NOT NULL DEFAULT 0,
  game_pid INTEGER NOT NULL DEFAULT 0,
  finished INTEGER NOT NULL DEFAULT 0,
  ended_at INTEGER NOT NULL DEFAULT 0,
  duration_seconds INTEGER NOT NULL DEFAULT 0,
  exit_code INTEGER NOT NULL DEFAULT -1,
  signal INTEGER NOT NULL DEFAULT 0,
  launch_error TEXT NOT NULL DEFAULT '',
  post_exit_code INTEGER,
  post_timed_out INTEGER NOT NULL DEFAULT 0,
  incomplete INTEGER NOT NULL DEFAULT 0,
  counted INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY(game_id, started_at)
) STRICT;
CREATE INDEX sessions_uncounted ON sessions(counted) WHERE counted = 0;
CREATE TABLE settings_snapshot(
  id INTEGER PRIMARY KEY CHECK(id = 1),
  saved_at INTEGER NOT NULL,
  toml TEXT NOT NULL
) STRICT;
CREATE TABLE ui_state(
  id INTEGER PRIMARY KEY CHECK(id = 1),
  state TEXT NOT NULL CHECK(json_valid(state))
) STRICT;
)sql",
};

constexpr std::array<std::string_view, 14> kTextColumns = {
    "id",       "install_path", "name",      "status",   "platform",   "source",        "exe_path",
    "args",     "working_dir",  "runner_ref", "source_ref", "data_dir", "installer_dir", "last_error"};
constexpr std::array<std::string_view, 4> kIntColumns = {"created_at", "updated_at", "play_seconds",
                                                         "last_session_at"};
constexpr std::array<std::string_view, 4> kJsonColumns = {"runner_config", "overrides", "env", "candidates"};

// Every column, in the order Write binds and ReadAll reads them.
std::vector<std::string_view> Columns() {
  std::vector<std::string_view> columns(kTextColumns.begin(), kTextColumns.end());
  columns.insert(columns.end(), kIntColumns.begin(), kIntColumns.end());
  columns.insert(columns.end(), kJsonColumns.begin(), kJsonColumns.end());
  columns.insert(columns.end(), {"confidence", "reviewed", "last_played_at"});
  return columns;
}

std::string Joined(const std::vector<std::string_view>& parts, std::string_view format) {
  std::string out;
  for (const std::string_view part : parts) {
    if (!out.empty()) out += ", ";
    out += std::vformat(format, std::make_format_args(part));
  }
  return out;
}

// Moves a damaged database and its WAL aside together.
void SetDatabaseAside(const fs::path& file) {
  const auto kept = SetAside(file);
  if (!kept) {
    log::Error("could not set {} aside: {}", file.string(), kept.error().message);
    return;
  }
  std::error_code ec;
  for (const char* suffix : {"-wal", "-shm"}) {
    fs::path from = file, to = *kept;
    from += suffix;
    to += suffix;
    if (fs::exists(from, ec)) fs::rename(from, to, ec);
  }
  log::Error("the library database {} is damaged; kept it as {}", file.string(), kept->string());
}
}  // namespace

GameStore::GameStore(std::filesystem::path file) : file_(std::move(file)), metadata_(file_.parent_path()) {}

Result<void> GameStore::Open() {
  fs::path backup = file_;
  backup += ".bak";
  std::error_code ec;
  const bool existed = fs::exists(file_, ec);
  auto opened = db_.Open(file_);
  if (opened && existed && !db_.Sound()) opened = Err("database_damaged", "the database failed its check");
  if (!opened && existed) {
    log::Error("could not open {}: {}", file_.string(), opened.error().message);
    db_.Close();
    SetDatabaseAside(file_);
    if (fs::exists(backup, ec)) {
      fs::copy_file(backup, file_, ec);
      log::Warn("restored the library from {}", backup.string());
    }
    opened = db_.Open(file_);
  }
  if (!opened) return opened;
  if (auto migrated = db_.Migrate(kMigrations); !migrated) return migrated;
  if (auto backed_up = db_.BackUp(backup); !backed_up) {
    log::Warn("could not back up the library: {}", backed_up.error().message);
  }
  return {};
}

// One-time move from games.toml (Mira 0.13 and earlier). Temporary: delete once users have upgraded.
void GameStore::ImportToml() {
  const fs::path toml_file = Dir() / "games.toml";
  std::error_code ec;
  if (!fs::exists(toml_file, ec)) return;
  if (!games_.empty()) {
    log::Warn("{} is left over beside a library that already has games; ignoring it", toml_file.string());
    return;
  }

  toml::parse_result parsed = toml::parse_file(toml_file.string());
  if (!parsed) {
    if (const auto broken = SetAside(toml_file)) {
      log::Error("games at {} could not be parsed ({}); kept it as {} and starting with an empty library",
                 toml_file.string(), parsed.error().description(), broken->string());
    } else {
      read_only_ = true;
      log::Error("games at {} could not be parsed ({}) or set aside ({}); not saving any changes",
                 toml_file.string(), parsed.error().description(), broken.error().message);
    }
    return;
  }

  const json whole = tomljson::ToJson(parsed.table());
  std::vector<model::Game> games;
  if (whole.contains("game") && whole["game"].is_array()) {
    for (const json& entry : whole["game"]) {
      model::Game game = model::GameFromJson(entry);
      if (game.id.empty()) {
        log::Warn("skipping a game entry in {} with no id", toml_file.string());
        continue;
      }
      games.push_back(std::move(game));
    }
  }
  const auto imported = Transact([&]() -> Result<void> {
    for (const model::Game& game : games) {
      if (auto written = Write(game); !written) return written;
    }
    return {};
  });
  if (!imported) {
    read_only_ = true;
    log::Error("could not import {}: {}; not saving any changes", toml_file.string(), imported.error().message);
    return;
  }
  fs::path done = toml_file;
  done += ".migrated";
  fs::rename(toml_file, done, ec);
  log::Info("imported {} game(s) from {}, kept as {}", games.size(), toml_file.string(), done.string());
}

void GameStore::ReadAll() {
  games_.clear();
  const std::vector<std::string_view> columns = Columns();
  auto select = db_.Prepare(std::format("SELECT {} FROM games ORDER BY rowid", Joined(columns, "{}")));
  if (!select) {
    log::Error("could not read the library: {}", select.error().message);
    return;
  }
  for (auto row = select->Step(); row && *row; row = select->Step()) {
    json document = json::object();
    int column = 0;
    for (const std::string_view name : kTextColumns) document[name] = select->Text(column++);
    for (const std::string_view name : kIntColumns) document[name] = select->Int(column++);
    for (const std::string_view name : kJsonColumns) {
      document[name] = json::parse(select->Text(column++), nullptr, /*allow_exceptions=*/false);
    }
    document["confidence"] = select->Real(column++);
    document["reviewed"] = select->Int(column++) != 0;
    if (!select->IsNull(column)) document["last_played_at"] = select->Int(column);
    games_.push_back(model::GameFromJson(document));
  }

  std::map<std::string, std::vector<std::string>> tags;
  auto tag_rows = db_.Prepare("SELECT game_id, tag FROM game_tags ORDER BY game_id, position");
  if (!tag_rows) return;
  for (auto row = tag_rows->Step(); row && *row; row = tag_rows->Step()) {
    tags[tag_rows->Text(0)].push_back(tag_rows->Text(1));
  }
  for (model::Game& game : games_) {
    if (auto it = tags.find(game.id); it != tags.end()) game.tags = std::move(it->second);
  }
}

void GameStore::Load() {
  metadata_.Load();
  std::lock_guard lock(mutex_);
  games_.clear();
  read_only_ = false;
  ++revision_;

  if (auto opened = Open(); !opened) {
    read_only_ = true;
    log::Error("could not open the library at {} ({}); not saving any changes", file_.string(),
               opened.error().message);
    return;
  }
  ReadAll();
  ImportToml();
  if (!read_only_) ReadAll();
  log::Info("loaded {} game(s) from {}", games_.size(), file_.string());
}

Result<void> GameStore::Write(const model::Game& game) {
  const std::vector<std::string_view> columns = Columns();
  std::string sql = std::format(
      "INSERT INTO games({}) VALUES({}) ON CONFLICT(id) DO UPDATE SET {}", Joined(columns, "{}"),
      Joined(std::vector<std::string_view>(columns.size(), "?"), "{}"), Joined(columns, "{0} = excluded.{0}"));
  auto insert = db_.Prepare(sql);
  if (!insert) return std::unexpected(insert.error());

  const json document = model::ToJson(game);
  int index = 1;
  for (const std::string_view name : kTextColumns) insert->Bind(index++, document[name].get<std::string>());
  for (const std::string_view name : kIntColumns) insert->Bind(index++, document[name].get<std::int64_t>());
  for (const std::string_view name : kJsonColumns) insert->Bind(index++, document[name].dump());
  insert->Bind(index++, game.confidence);
  insert->Bind(index++, std::int64_t{game.reviewed});
  if (game.last_played_at) {
    insert->Bind(index, *game.last_played_at);
  } else {
    insert->BindNull(index);
  }
  if (auto done = insert->Run(); !done) return done;

  auto clear = db_.Prepare("DELETE FROM game_tags WHERE game_id = ?");
  if (!clear) return std::unexpected(clear.error());
  if (auto done = clear->Bind(1, game.id).Run(); !done) return done;
  std::int64_t position = 0;
  for (const std::string& tag : game.tags) {
    auto add = db_.Prepare("INSERT OR IGNORE INTO game_tags(game_id, position, tag) VALUES(?, ?, ?)");
    if (!add) return std::unexpected(add.error());
    if (auto done = add->Bind(1, game.id).Bind(2, position++).Bind(3, tag).Run(); !done) return done;
  }
  return {};
}

Result<void> GameStore::Delete(const std::string& id) {
  auto remove = db_.Prepare("DELETE FROM games WHERE id = ?");
  if (!remove) return std::unexpected(remove.error());
  return remove->Bind(1, id).Run();
}

Result<void> GameStore::Transact(const std::function<Result<void>()>& write) {
  ++revision_;
  if (read_only_) {
    return Err("games_read_only", std::format("the library at {} couldn't be opened or imported", file_.string()),
               "See mirad's log for why, then restart mirad.");
  }
  Transaction transaction(db_);
  if (auto begun = transaction.Begin(); !begun) return begun;
  if (auto written = write(); !written) return written;
  return transaction.Commit();
}

std::string GameStore::SettingsSnapshot() {
  std::lock_guard lock(mutex_);
  if (!db_.IsOpen()) return {};
  auto select = db_.Prepare("SELECT toml FROM settings_snapshot WHERE id = 1");
  if (!select) return {};
  auto row = select->Step();
  return row && *row ? select->Text(0) : std::string();
}

void GameStore::KeepSettingsSnapshot(const std::string& toml) {
  std::lock_guard lock(mutex_);
  if (!db_.IsOpen()) return;
  auto keep = db_.Prepare("INSERT OR REPLACE INTO settings_snapshot(id, saved_at, toml) VALUES(1, ?, ?)");
  if (!keep) return;
  if (auto done = keep->Bind(1, model::NowSeconds()).Bind(2, toml).Run(); !done) {
    log::Warn("could not keep a copy of the settings: {}", done.error().message);
  }
}

json GameStore::UiState() {
  std::lock_guard lock(mutex_);
  if (!db_.IsOpen()) return json::object();
  auto select = db_.Prepare("SELECT state FROM ui_state WHERE id = 1");
  if (!select) return json::object();
  auto row = select->Step();
  if (!row || !*row) return json::object();
  json state = json::parse(select->Text(0), nullptr, false);
  return state.is_object() ? state : json::object();
}

void GameStore::KeepUiState(const json& state) {
  std::lock_guard lock(mutex_);
  if (!db_.IsOpen()) return;
  auto keep = db_.Prepare("INSERT OR REPLACE INTO ui_state(id, state) VALUES(1, ?)");
  if (!keep) return;
  if (auto done = keep->Bind(1, state.dump()).Run(); !done) {
    log::Warn("could not save the window state: {}", done.error().message);
  }
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
  std::lock_guard lock(mutex_);
  if (auto written = Transact([&] { return Write(game); }); !written) return written;
  auto it = std::ranges::find(games_, game.id, &model::Game::id);
  if (it == games_.end()) {
    games_.push_back(std::move(game));
  } else {
    *it = std::move(game);
  }
  return {};
}

Result<void> GameStore::Merge(const std::optional<model::Game>& base, model::Game& game) {
  std::lock_guard lock(mutex_);
  auto it = std::ranges::find(games_, game.id, &model::Game::id);
  model::Game merged = game;
  if (it != games_.end() && base) {
    const nlohmann::json before = model::ToJson(*base);
    const nlohmann::json edited = model::ToJson(game);
    nlohmann::json live = model::ToJson(*it);
    for (const auto& [key, value] : edited.items()) {
      if (key != "tags" && before.value(key, nlohmann::json()) != value) live[key] = value;
    }
    merged = model::GameFromJson(live);
    for (const std::string& tag : game.tags) {
      if (std::ranges::find(base->tags, tag) == base->tags.end() && std::ranges::find(merged.tags, tag) == merged.tags.end()) {
        merged.tags.push_back(tag);
      }
    }
    std::erase_if(merged.tags, [&](const std::string& tag) {
      return std::ranges::find(base->tags, tag) != base->tags.end() && std::ranges::find(game.tags, tag) == game.tags.end();
    });
  }
  if (auto written = Transact([&] { return Write(merged); }); !written) return written;
  if (it == games_.end()) {
    games_.push_back(merged);
  } else {
    *it = merged;
  }
  game = std::move(merged);
  return {};
}

Result<model::Game> GameStore::Update(const std::string& id,
                                       std::function<void(model::Game&)> mutator) {
  std::lock_guard lock(mutex_);
  auto it = std::ranges::find(games_, id, &model::Game::id);
  if (it == games_.end()) return Err("game_not_found", std::format("no game with id \"{}\"", id));
  model::Game updated = *it;
  mutator(updated);
  updated.updated_at = model::NowSeconds();
  if (auto written = Transact([&] { return Write(updated); }); !written) return std::unexpected(written.error());
  *it = updated;
  return updated;
}

Result<model::Game> GameStore::FinishSession(const PlaySession& session,
                                              std::function<void(model::Game&)> mutator) {
  std::lock_guard lock(mutex_);
  auto it = std::ranges::find(games_, session.game_id, &model::Game::id);
  if (it == games_.end()) return Err("game_not_found", std::format("no game with id \"{}\"", session.game_id));
  model::Game updated = *it;
  mutator(updated);
  updated.updated_at = model::NowSeconds();
  const auto written = Transact([&]() -> Result<void> {
    if (auto done = Write(updated); !done) return done;
    auto insert = db_.Prepare(
        "INSERT INTO sessions(game_id, started_at, ended_at, duration_seconds, exit_code, signal, incomplete, "
        "finished, counted) VALUES(?, ?, ?, ?, ?, ?, ?, 1, 1) "
        "ON CONFLICT(game_id, started_at) DO UPDATE SET ended_at = excluded.ended_at, "
        "duration_seconds = excluded.duration_seconds, exit_code = excluded.exit_code, signal = excluded.signal, "
        "incomplete = excluded.incomplete, finished = 1, counted = 1");
    if (!insert) return std::unexpected(insert.error());
    return insert->Bind(1, session.game_id)
        .Bind(2, session.started_at)
        .Bind(3, session.ended_at)
        .Bind(4, session.duration_seconds)
        .Bind(5, std::int64_t{session.exit_code})
        .Bind(6, std::int64_t{session.signal})
        .Bind(7, std::int64_t{session.incomplete})
        .Run();
  });
  if (!written) return std::unexpected(written.error());
  *it = updated;
  return updated;
}

std::vector<PlaySession> GameStore::Sessions(const std::string& id, int limit) const {
  std::lock_guard lock(mutex_);
  std::vector<PlaySession> sessions;
  if (!db_.IsOpen()) return sessions;
  auto select = db_.Prepare(
      "SELECT started_at, ended_at, duration_seconds, exit_code, signal, incomplete FROM sessions "
      "WHERE game_id = ? AND finished = 1 ORDER BY started_at DESC LIMIT ?");
  if (!select) return sessions;
  select->Bind(1, id).Bind(2, std::int64_t{limit});
  for (auto row = select->Step(); row && *row; row = select->Step()) {
    sessions.push_back({.game_id = id,
                        .started_at = select->Int(0),
                        .ended_at = select->Int(1),
                        .duration_seconds = select->Int(2),
                        .exit_code = static_cast<int>(select->Int(3)),
                        .signal = static_cast<int>(select->Int(4)),
                        .incomplete = select->Int(5) != 0});
  }
  return sessions;
}

Result<std::vector<model::Game>> GameStore::UpdateMany(const std::vector<std::string>& ids,
                                                       std::function<bool(model::Game&)> mutator) {
  std::lock_guard lock(mutex_);
  std::vector<model::Game> updated;
  const std::set<std::string> wanted(ids.begin(), ids.end());
  for (const model::Game& game : games_) {
    if (!wanted.contains(game.id)) continue;
    model::Game copy = game;
    if (!mutator(copy)) continue;
    copy.updated_at = model::NowSeconds();
    updated.push_back(std::move(copy));
  }
  if (updated.empty()) return updated;
  const auto written = Transact([&]() -> Result<void> {
    for (const model::Game& game : updated) {
      if (auto done = Write(game); !done) return done;
    }
    return {};
  });
  if (!written) return std::unexpected(written.error());
  for (const model::Game& game : updated) *std::ranges::find(games_, game.id, &model::Game::id) = game;
  return updated;
}

Result<std::vector<std::string>> GameStore::RemoveMany(const std::vector<std::string>& ids) {
  std::lock_guard lock(mutex_);
  std::vector<std::string> removed;
  for (const model::Game& game : games_) {
    if (std::ranges::find(ids, game.id) != ids.end()) removed.push_back(game.id);
  }
  if (removed.empty()) return removed;
  const auto deleted = Transact([&]() -> Result<void> {
    for (const std::string& id : removed) {
      if (auto done = Delete(id); !done) return done;
    }
    return {};
  });
  if (!deleted) return std::unexpected(deleted.error());
  std::erase_if(games_, [&](const model::Game& game) { return std::ranges::find(removed, game.id) != removed.end(); });
  return removed;
}

Result<void> GameStore::Remove(const std::string& id) {
  std::lock_guard lock(mutex_);
  if (std::ranges::find(games_, id, &model::Game::id) == games_.end()) {
    return Err("game_not_found", std::format("no game with id \"{}\"", id));
  }
  if (auto deleted = Transact([&] { return Delete(id); }); !deleted) return deleted;
  std::erase_if(games_, [&](const model::Game& game) { return game.id == id; });
  return {};
}

}  // namespace mira::store
