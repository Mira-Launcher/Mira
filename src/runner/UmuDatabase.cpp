#include "runner/UmuDatabase.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <string_view>
#include <vector>

#include "config/Config.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "library/GameIdentity.h"
#include "runner/Curl.h"

namespace mira::runner {
namespace {

namespace fs = std::filesystem;

constexpr std::chrono::hours kMaxAge{24 * 7};

// Stores protonfixes keeps a folder of fixes for; any other runs as "umu".
constexpr std::array<std::string_view, 10> kFixStores = {
    "amazon", "battlenet", "ea",    "egs",     "gog",
    "humble", "itchio",    "steam", "ubisoft", "zoomplatform"};

struct Row {
  std::string title;
  std::string store;
  std::string codename;
  std::string umu_id;
};

struct Database {
  fs::path file;
  fs::file_time_type loaded_at{};
  std::map<std::pair<std::string, std::string>, Row> by_code;  // (store, codename)
  std::map<std::string, std::vector<Row>> by_title;            // Slugify(title)
};

std::mutex& DatabaseMutex() {
  static std::mutex mutex;
  return mutex;
}

fs::path DatabaseFile() {
  return paths::EnvOr("XDG_CACHE_HOME", paths::Home() / ".cache") / "mira" / "umu-database.csv";
}

// One CSV line's fields; quoted fields may hold commas and "" for a quote.
std::vector<std::string> CsvFields(const std::string& line) {
  std::vector<std::string> fields(1);
  bool quoted = false;
  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (quoted && c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
      fields.back() += '"';
      ++i;
    } else if (c == '"') {
      quoted = !quoted;
    } else if (c == ',' && !quoted) {
      fields.emplace_back();
    } else if (c != '\r') {
      fields.back() += c;
    }
  }
  return fields;
}

// The database, reread when the file on disk changes. Callers hold DatabaseMutex().
const Database& Load() {
  static Database db;
  std::error_code ec;
  const fs::path file = DatabaseFile();
  const auto modified = fs::last_write_time(file, ec);
  if (ec) db = Database{};
  if (ec || (file == db.file && modified == db.loaded_at)) return db;
  db = Database{};
  db.file = file;
  db.loaded_at = modified;
  std::ifstream in(file);
  std::string line;
  std::getline(in, line);  // the header
  while (std::getline(in, line)) {
    std::vector<std::string> f = CsvFields(line);
    if (f.size() < 4 || !f[3].starts_with("umu-")) continue;
    Row row{.title = strings::Trim(f[0]), .store = f[1], .codename = f[2], .umu_id = f[3]};
    if (row.store != "none") db.by_code.emplace(std::pair{row.store, row.codename}, row);
    db.by_title[strings::Slugify(row.title)].push_back(std::move(row));
  }
  return db;
}

}  // namespace

void RefreshUmuDatabase(const config::Config& config) {
  if (!config.GetBool("umu.match")) return;
  if (auto got =
          RefreshDownload(config.GetString("umu.database_url"), DatabaseFile(), kMaxAge, 16 << 20);
      !got)
    log::Warn("could not download the umu database: {}", got.error().message);
}

std::optional<UmuRow> UmuRowFor(const config::Config& config, const std::string& store,
                                const std::string& code) {
  if (!config.GetBool("umu.match") || store.empty() || code.empty()) return std::nullopt;
  const std::lock_guard lock(DatabaseMutex());
  const Database& db = Load();
  const auto it = db.by_code.find({store, code});
  if (it == db.by_code.end()) return std::nullopt;
  return UmuRow{.umu_id = it->second.umu_id, .title = it->second.title};
}

std::optional<UmuRow> UmuRowByTitle(const config::Config& config, const std::string& title) {
  if (!config.GetBool("umu.match") || title.empty()) return std::nullopt;
  const std::lock_guard lock(DatabaseMutex());
  const Database& db = Load();
  const auto it = db.by_title.find(strings::Slugify(title));
  if (it == db.by_title.end()) return std::nullopt;
  // Only when every row with that title is the same game.
  const std::vector<Row>& rows = it->second;
  if (std::ranges::any_of(rows, [&](const Row& r) { return r.umu_id != rows[0].umu_id; }))
    return std::nullopt;
  return UmuRow{.umu_id = rows[0].umu_id, .title = rows[0].title};
}

UmuMatch UmuIdentity(const config::Config& config, const model::Game& game) {
  const auto text = [&](const char* key) {
    const auto it = game.runner_config.find(key);
    return it != game.runner_config.end() && it->is_string() ? it->get<std::string>()
                                                             : std::string();
  };
  UmuMatch env{.gameid = text("gameid"), .store = text("store"), .title = {}};
  const library::GameIdentity identity = library::Identify(config, game);
  if (env.gameid.empty()) {
    env.gameid = identity.umu_id;
    env.title = identity.title;
  }
  // A Steam game's fixes are found by its GAMEID alone, as before.
  if (env.store.empty() && identity.store != "steam") env.store = identity.store;
  // Without a GAMEID every game runs as umu-default, so Proton gives all of their windows the same
  // class (steam_app_default) and desktops group them as one app. umu only accepts letters, digits
  // and underscores after "umu-".
  if (env.gameid.empty() || env.gameid == "none") {
    env.gameid = "umu-mira_";
    for (const char ch : game.id) {
      if (std::isalnum(static_cast<unsigned char>(ch))) env.gameid.push_back(ch);
    }
  }
  return env;
}

bool HasProtonFix(const fs::path& build_dir, const UmuMatch& identity) {
  const std::string& id = identity.gameid;
  if (id.empty() || id.starts_with("umu-mira_")) return false;
  // protonfixes' own lookup: a bare number is a Steam appid, else the store's folder.
  std::string store = std::ranges::all_of(id, [](unsigned char c) { return std::isdigit(c); })
                          ? "steam"
                          : identity.store;
  if (!std::ranges::contains(kFixStores, store)) store = "umu";
  std::error_code ec;
  return fs::exists(build_dir / "protonfixes" / ("gamefixes-" + store) / (id + ".py"), ec);
}

std::string SteamAppId(const std::string& gameid) {
  const std::string digits = gameid.starts_with("umu-") ? gameid.substr(4) : gameid;
  if (digits.empty() ||
      !std::ranges::all_of(digits, [](unsigned char c) { return std::isdigit(c); }))
    return "";
  return digits;
}

}  // namespace mira::runner
