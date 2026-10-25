#include "store/MetadataStore.h"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <set>
#include <utility>

#include "core/Log.h"
#include "model/Types.h"

namespace mira::store {
namespace {
using nlohmann::json;
namespace fs = std::filesystem;

constexpr std::array<std::string_view, 7> kMigrations = {
    R"sql(
CREATE TABLE metadata(
  id TEXT PRIMARY KEY,
  info TEXT NOT NULL CHECK(json_valid(info)),
  updated_at INTEGER NOT NULL
) STRICT;
CREATE TABLE artwork(
  id TEXT NOT NULL REFERENCES metadata ON DELETE CASCADE,
  slot TEXT NOT NULL CHECK(slot IN ('cover', 'hero', 'logo', 'icon', 'capsule', 'header')),
  file TEXT NOT NULL,
  content_type TEXT NOT NULL,
  version TEXT NOT NULL,
  PRIMARY KEY(id, slot)
) STRICT;
-- What's fetched for the whole library rather than one id, such as Steam's tag names.
CREATE TABLE lists(
  name TEXT PRIMARY KEY,
  value TEXT NOT NULL CHECK(json_valid(value)),
  updated_at INTEGER NOT NULL
) STRICT;
)sql",
    // Art saved before downloads were shrunk, fitted once in the background.
    R"sql(
CREATE TABLE fit_pending(id TEXT PRIMARY KEY) STRICT;
INSERT INTO fit_pending SELECT id FROM metadata;
)sql",
    // What each store last said the account owns, in its order, so a listing answers at once.
    R"sql(
CREATE TABLE catalog(
  source TEXT NOT NULL,
  position INTEGER NOT NULL,
  ref TEXT NOT NULL,
  title TEXT NOT NULL,
  play_seconds INTEGER NOT NULL,
  owned INTEGER NOT NULL,
  PRIMARY KEY(source, position)
) STRICT;
CREATE TABLE catalog_checked(source TEXT PRIMARY KEY, checked_at INTEGER NOT NULL) STRICT;
)sql",
    // Each title's store page, filled in as each store is listed again; found by ref for a game.
    R"sql(
ALTER TABLE catalog ADD COLUMN url TEXT NOT NULL DEFAULT '';
CREATE INDEX catalog_ref ON catalog(source, ref);
)sql",
    // Each game's achievements as its store last listed them.
    R"sql(
CREATE TABLE achievements(
  id TEXT PRIMARY KEY,
  fetched_at INTEGER NOT NULL,
  json TEXT NOT NULL CHECK(json_valid(json))
) STRICT;
)sql",
    // Ludusavi's save-location manifest, indexed. A rebuild writes the next generation beside the
    // current one, which the lists row "save_entries" names.
    R"sql(
CREATE TABLE save_entries(
  generation INTEGER NOT NULL,
  name TEXT NOT NULL,
  norm TEXT NOT NULL,
  alias_of TEXT NOT NULL,
  entry TEXT NOT NULL,
  PRIMARY KEY(generation, name)
) STRICT;
CREATE INDEX save_entries_norm ON save_entries(generation, norm, name);
CREATE TABLE save_ids(
  generation INTEGER NOT NULL,
  kind TEXT NOT NULL,
  ref TEXT NOT NULL,
  name TEXT NOT NULL
) STRICT;
CREATE INDEX save_ids_ref ON save_ids(generation, kind, ref, name);
)sql",
    // Where each GOG or Epic game keeps its cloud saves, as its store last said.
    R"sql(
CREATE TABLE cloud_saves(
  id TEXT PRIMARY KEY,
  fetched_at INTEGER NOT NULL,
  json TEXT NOT NULL CHECK(json_valid(json))
) STRICT;
)sql",
};

constexpr std::string_view kSaveIndexList = "save_entries";
// Rows written or deleted per hold of the lock while the save index is rebuilt.
constexpr std::size_t kSaveBatch = 2000;

// API slot name, and the key the fetcher keeps it under in the info. Steam's
// capsule and header are served too, but left out of ArtVersions' tiles.
constexpr std::array<std::pair<std::string_view, std::string_view>, 6> kSlots = {{
    {"cover", "artwork"},
    {"hero", "hero"},
    {"logo", "logo"},
    {"icon", "icon"},
    {"capsule", "capsule"},
    {"header", "header"},
}};

bool InArtVersions(std::string_view slot) {
  return slot != "capsule" && slot != "header";
}

std::string_view InfoKey(std::string_view slot) {
  for (const auto& [name, key] : kSlots) {
    if (name == slot) return key;
  }
  return {};
}

// A slot's file keeps its name when replaced, so its time and size stand in for its contents.
std::string FileVersion(const fs::path& file) {
  std::error_code ec;
  const auto written = fs::last_write_time(file, ec);
  if (ec) return {};
  const auto size = fs::file_size(file, ec);
  if (ec) return {};
  return std::format("{:x}-{:x}", static_cast<std::uint64_t>(written.time_since_epoch().count()),
                     size);
}

std::string FileName(const json& slot) {
  const std::string name = slot.is_object() ? slot.value("file", std::string()) : std::string();
  // Only a plain name inside the id's folder.
  return name.find('/') == std::string::npos && name != ".." ? name : std::string();
}
}  // namespace

MetadataStore::MetadataStore(fs::path dir) : dir_(std::move(dir)) {}

fs::path MetadataStore::ArtworkDir(const std::string& id) const {
  return dir_ / "artwork" / id;
}

Result<void> MetadataStore::Open() {
  const fs::path file = dir_ / "cache.db";
  auto opened = db_.Open(file);
  if (opened) opened = db_.CheckSound();
  if (!opened && opened.error().code == "database_damaged") {
    // Everything in it can be fetched again.
    log::Warn("the metadata cache {} is damaged ({}); starting it over", file.string(),
              opened.error().message);
    db_.Close();
    std::error_code ec;
    for (const char* suffix : {"", "-wal", "-shm"}) fs::remove(file.string() + suffix, ec);
    opened = db_.Open(file);
  }
  if (!opened) return opened;
  return db_.Migrate(kMigrations);
}

void MetadataStore::Load() {
  std::lock_guard lock(mutex_);
  versions_.clear();
  if (auto opened = Open(); !opened) {
    log::Error("could not open the metadata cache: {}", opened.error().message);
    db_.Close();
    return;
  }

  auto rows = db_.Prepare("SELECT id, slot, version FROM artwork");
  if (!rows) {
    log::Error("could not read the art versions: {}", rows.error().message);
    return;
  }
  for (auto row = rows->Step(); row && *row; row = rows->Step()) {
    if (InArtVersions(rows->Text(1))) versions_[rows->Text(0)][rows->Text(1)] = rows->Text(2);
  }

  tiers_.clear();
  auto tiers = db_.Prepare(
      "SELECT id, info->>'$.protondb.tier' FROM metadata WHERE info->>'$.protondb.tier' != ''");
  if (!tiers) {
    log::Error("could not read the ProtonDB tiers: {}", tiers.error().message);
    return;
  }
  for (auto row = tiers->Step(); row && *row; row = tiers->Step())
    tiers_[tiers->Text(0)] = tiers->Text(1);

  save_generation_ = 0;
  save_count_ = 0;
  if (auto index = db_.Prepare("SELECT value FROM lists WHERE name = ?")) {
    if (auto row = index->Bind(1, kSaveIndexList).Step(); row && *row) {
      const json value = json::parse(index->Text(0), nullptr, false);
      if (value.is_object()) {
        save_generation_ = value.value("generation", std::int64_t{0});
        save_count_ = value.value("games", std::int64_t{0});
      }
    }
  }
}

json MetadataStore::Read(const std::string& id) const {
  std::lock_guard lock(mutex_);
  if (!db_.IsOpen()) return json::object();
  auto select = db_.Prepare("SELECT info FROM metadata WHERE id = ?");
  if (!select) return json::object();
  select->Bind(1, id);
  auto row = select->Step();
  if (!row || !*row) return json::object();
  json info = json::parse(select->Text(0), nullptr, false);
  return info.is_object() ? info : json::object();
}

bool MetadataStore::Has(const std::string& id) const {
  std::lock_guard lock(mutex_);
  if (!db_.IsOpen()) return false;
  auto select = db_.Prepare("SELECT 1 FROM metadata WHERE id = ?");
  if (!select) return false;
  select->Bind(1, id);
  auto row = select->Step();
  return row && *row;
}

std::unordered_map<std::string, json> MetadataStore::Field(std::string_view key) const {
  std::unordered_map<std::string, json> out;
  std::lock_guard lock(mutex_);
  if (!db_.IsOpen()) return out;
  // `->` gives JSON text for any value (json_extract would give a string unquoted); json_quote
  // matches the key as written whatever it contains.
  auto select = db_.Prepare("SELECT id, info -> ('$.' || json_quote(?)) FROM metadata");
  if (!select) return out;
  select->Bind(1, std::string(key));
  for (auto row = select->Step(); row && *row; row = select->Step()) {
    out[select->Text(0)] =
        select->IsNull(1) ? json() : json::parse(select->Text(1), nullptr, false);
  }
  return out;
}

json MetadataStore::ReadList(const std::string& name) const {
  std::lock_guard lock(mutex_);
  if (!db_.IsOpen()) return json();
  auto select = db_.Prepare("SELECT value FROM lists WHERE name = ?");
  if (!select) return json();
  select->Bind(1, name);
  auto row = select->Step();
  if (!row || !*row) return json();
  json value = json::parse(select->Text(0), nullptr, false);
  return value.is_discarded() ? json() : value;
}

Result<void> MetadataStore::WriteList(const std::string& name, const json& value) {
  std::lock_guard lock(mutex_);
  if (!db_.IsOpen()) return Err("metadata_write_failed", "the metadata cache isn't open");
  auto upsert = db_.Prepare(
      "INSERT INTO lists(name, value, updated_at) VALUES(?, ?, ?) "
      "ON CONFLICT(name) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at");
  if (!upsert) return std::unexpected(upsert.error());
  return upsert->Bind(1, name)
      .Bind(2, value.dump(-1, ' ', false, json::error_handler_t::replace))
      .Bind(3, model::NowSeconds())
      .Run();
}

json MetadataStore::ReadAchievements(const std::string& id) const {
  std::lock_guard lock(mutex_);
  if (!db_.IsOpen()) return json();
  auto select = db_.Prepare("SELECT json FROM achievements WHERE id = ?");
  if (!select) return json();
  auto row = select->Bind(1, id).Step();
  if (!row || !*row) return json();
  json value = json::parse(select->Text(0), nullptr, false);
  return value.is_discarded() ? json() : value;
}

Result<void> MetadataStore::WriteAchievements(const std::string& id, std::int64_t fetched_at,
                                              const json& value) {
  std::lock_guard lock(mutex_);
  if (!db_.IsOpen()) return Err("metadata_write_failed", "the metadata cache isn't open");
  auto upsert = db_.Prepare(
      "INSERT INTO achievements(id, fetched_at, json) VALUES(?, ?, ?) "
      "ON CONFLICT(id) DO UPDATE SET fetched_at = excluded.fetched_at, json = excluded.json");
  if (!upsert) return std::unexpected(upsert.error());
  return upsert->Bind(1, id)
      .Bind(2, fetched_at)
      .Bind(3, value.dump(-1, ' ', false, json::error_handler_t::replace))
      .Run();
}

void MetadataStore::DropGameRowsLocked(const std::string& id) {
  if (!db_.IsOpen()) return;
  for (const char* table : {"achievements", "cloud_saves"}) {
    if (auto remove = db_.Prepare(std::format("DELETE FROM {} WHERE id = ?", table))) {
      if (auto done = remove->Bind(1, id).Run(); !done) {
        log::Warn("could not drop {}'s {}: {}", id, table, done.error().message);
      }
    }
  }
}

json MetadataStore::ReadCloudSaves(const std::string& id) const {
  std::lock_guard lock(mutex_);
  if (!db_.IsOpen()) return json();
  auto select = db_.Prepare("SELECT fetched_at, json FROM cloud_saves WHERE id = ?");
  if (!select) return json();
  auto row = select->Bind(1, id).Step();
  if (!row || !*row) return json();
  json value = json::parse(select->Text(1), nullptr, false);
  if (!value.is_object()) return json();
  value["fetched_at"] = select->Int(0);
  return value;
}

Result<void> MetadataStore::WriteCloudSaves(const std::string& id, const json& value) {
  std::lock_guard lock(mutex_);
  if (!db_.IsOpen()) return Err("metadata_write_failed", "the metadata cache isn't open");
  auto upsert = db_.Prepare(
      "INSERT INTO cloud_saves(id, fetched_at, json) VALUES(?, ?, ?) "
      "ON CONFLICT(id) DO UPDATE SET fetched_at = excluded.fetched_at, json = excluded.json");
  if (!upsert) return std::unexpected(upsert.error());
  return upsert->Bind(1, id).Bind(2, model::NowSeconds()).Bind(3, value.dump()).Run();
}

Result<void> MetadataStore::ReplaceSaveEntries(std::span<const SaveEntry> entries) {
  std::lock_guard rebuild(save_rebuild_mutex_);
  std::int64_t generation = 0;
  {
    std::lock_guard lock(mutex_);
    if (!db_.IsOpen()) return Err("metadata_write_failed", "the metadata cache isn't open");
    generation = save_generation_ + 1;
  }
  // What an earlier rebuild left when mirad stopped halfway.
  DropSaveGenerations(generation - 1);

  for (std::size_t start = 0; start < entries.size(); start += kSaveBatch) {
    std::lock_guard lock(mutex_);
    Transaction transaction(db_);
    if (auto begun = transaction.Begin(); !begun) return std::unexpected(begun.error());
    auto add_entry = db_.Prepare(
        "INSERT OR IGNORE INTO save_entries(generation, name, norm, alias_of, entry) "
        "VALUES(?, ?, ?, ?, ?)");
    if (!add_entry) return std::unexpected(add_entry.error());
    auto add_id =
        db_.Prepare("INSERT INTO save_ids(generation, kind, ref, name) VALUES(?, ?, ?, ?)");
    if (!add_id) return std::unexpected(add_id.error());
    for (const SaveEntry& entry :
         entries.subspan(start, std::min(kSaveBatch, entries.size() - start))) {
      if (auto done = add_entry->Bind(1, generation)
                          .Bind(2, entry.name)
                          .Bind(3, entry.norm)
                          .Bind(4, entry.alias_of)
                          .Bind(5, entry.entry)
                          .Run();
          !done)
        return std::unexpected(done.error());
      for (const auto& [kind, ref] : entry.ids) {
        if (auto done =
                add_id->Bind(1, generation).Bind(2, kind).Bind(3, ref).Bind(4, entry.name).Run();
            !done)
          return std::unexpected(done.error());
      }
    }
    if (auto committed = transaction.Commit(); !committed)
      return std::unexpected(committed.error());
  }

  const auto games =
      std::ranges::count_if(entries, [](const SaveEntry& e) { return e.alias_of.empty(); });
  {
    std::lock_guard lock(mutex_);
    auto upsert = db_.Prepare(
        "INSERT INTO lists(name, value, updated_at) VALUES(?, ?, ?) "
        "ON CONFLICT(name) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at");
    if (!upsert) return std::unexpected(upsert.error());
    if (auto done = upsert->Bind(1, kSaveIndexList)
                        .Bind(2, json{{"generation", generation}, {"games", games}}.dump())
                        .Bind(3, model::NowSeconds())
                        .Run();
        !done)
      return std::unexpected(done.error());
    save_generation_ = generation;
    save_count_ = games;
  }
  DropSaveGenerations(generation);
  return {};
}

void MetadataStore::DropSaveGenerations(std::int64_t keep) {
  for (const char* sql :
       {"DELETE FROM save_entries WHERE rowid IN (SELECT rowid FROM save_entries WHERE "
        "generation != ? LIMIT ?)",
        "DELETE FROM save_ids WHERE rowid IN (SELECT rowid FROM save_ids WHERE generation != ? "
        "LIMIT ?)"}) {
    for (;;) {
      std::lock_guard lock(mutex_);
      if (!db_.IsOpen()) return;
      auto remove = db_.Prepare(sql);
      if (!remove) return;
      if (auto done = remove->Bind(1, keep).Bind(2, static_cast<std::int64_t>(kSaveBatch)).Run();
          !done) {
        log::Warn("could not clear the old save index: {}", done.error().message);
        return;
      }
      auto changes = db_.Prepare("SELECT changes()");
      if (!changes) return;
      if (auto row = changes->Step();
          !row || !*row || changes->Int(0) < static_cast<std::int64_t>(kSaveBatch))
        break;
    }
  }
}

std::optional<MetadataStore::SaveEntry> MetadataStore::FindSaveEntry(std::string_view name) const {
  std::lock_guard lock(mutex_);
  if (!db_.IsOpen()) return std::nullopt;
  auto select = db_.Prepare(
      "SELECT norm, alias_of, entry FROM save_entries WHERE generation = ? AND name = ?");
  if (!select) return std::nullopt;
  auto row = select->Bind(1, save_generation_).Bind(2, name).Step();
  if (!row || !*row) return std::nullopt;
  return SaveEntry{std::string(name), select->Text(0), select->Text(1), select->Text(2)};
}

std::string MetadataStore::SaveEntryWithId(std::string_view kind, std::string_view ref) const {
  std::lock_guard lock(mutex_);
  if (!db_.IsOpen()) return {};
  // Several entries can share an id (a game and its demo, say); the first by name is taken.
  auto select = db_.Prepare(
      "SELECT name FROM save_ids WHERE generation = ? AND kind = ? AND ref = ? ORDER BY name "
      "LIMIT 1");
  if (!select) return {};
  auto row = select->Bind(1, save_generation_).Bind(2, kind).Bind(3, ref).Step();
  return row && *row ? select->Text(0) : std::string();
}

std::string MetadataStore::SaveEntryWithNorm(std::string_view norm) const {
  std::lock_guard lock(mutex_);
  if (!db_.IsOpen() || norm.empty()) return {};
  auto select = db_.Prepare(
      "SELECT name FROM save_entries WHERE generation = ? AND norm = ? ORDER BY name LIMIT 1");
  if (!select) return {};
  auto row = select->Bind(1, save_generation_).Bind(2, norm).Step();
  return row && *row ? select->Text(0) : std::string();
}

std::vector<MetadataStore::SaveEntry> MetadataStore::SearchSaveEntries(std::string_view norm,
                                                                       int limit) const {
  std::lock_guard lock(mutex_);
  std::vector<SaveEntry> found;
  if (!db_.IsOpen() || norm.empty()) return found;
  auto select = db_.Prepare(
      "SELECT name, norm, alias_of FROM save_entries WHERE generation = ?1 AND instr(norm, ?2) > 0 "
      "ORDER BY instr(norm, ?2) != 1, length(name), name LIMIT ?3");
  if (!select) return found;
  select->Bind(1, save_generation_).Bind(2, norm).Bind(3, std::int64_t{limit});
  for (auto row = select->Step(); row && *row; row = select->Step())
    found.push_back({select->Text(0), select->Text(1), select->Text(2), {}});
  return found;
}

std::int64_t MetadataStore::SaveEntryCount() const {
  std::lock_guard lock(mutex_);
  return save_count_;
}

std::optional<std::vector<MetadataStore::CatalogRow>> MetadataStore::ReadCatalog(
    const std::string& source) const {
  std::lock_guard lock(mutex_);
  return ReadCatalogLocked(source);
}

std::int64_t MetadataStore::CatalogCheckedAt(const std::string& source) const {
  std::lock_guard lock(mutex_);
  if (!db_.IsOpen()) return 0;
  auto select = db_.Prepare("SELECT checked_at FROM catalog_checked WHERE source = ?");
  if (!select) return 0;
  auto row = select->Bind(1, source).Step();
  return row && *row ? select->Int(0) : 0;
}

std::optional<std::vector<MetadataStore::CatalogRow>> MetadataStore::ReadCatalogLocked(
    const std::string& source) const {
  if (!db_.IsOpen()) return std::nullopt;
  auto checked = db_.Prepare("SELECT 1 FROM catalog_checked WHERE source = ?");
  if (!checked) return std::nullopt;
  checked->Bind(1, source);
  if (auto row = checked->Step(); !row || !*row) return std::nullopt;

  auto select = db_.Prepare(
      "SELECT ref, title, play_seconds, owned, url FROM catalog WHERE source = ? ORDER BY "
      "position");
  if (!select) return std::nullopt;
  select->Bind(1, source);
  std::vector<CatalogRow> rows;
  for (auto row = select->Step(); row && *row; row = select->Step()) {
    rows.push_back(
        {select->Text(0), select->Text(1), select->Int(2), select->Int(3) != 0, select->Text(4)});
  }
  return rows;
}

std::string MetadataStore::CatalogUrl(const std::string& source, const std::string& ref) const {
  std::lock_guard lock(mutex_);
  if (!db_.IsOpen()) return {};
  auto select = db_.Prepare("SELECT url FROM catalog WHERE source = ? AND ref = ? LIMIT 1");
  if (!select) return {};
  auto row = select->Bind(1, source).Bind(2, ref).Step();
  return row && *row ? select->Text(0) : std::string();
}

Result<bool> MetadataStore::WriteCatalog(const std::string& source,
                                         const std::vector<CatalogRow>& rows) {
  std::lock_guard lock(mutex_);
  if (!db_.IsOpen()) return Err("metadata_write_failed", "the metadata cache isn't open");
  const auto before = ReadCatalogLocked(source);
  const bool changed = !before || *before != rows;
  Transaction transaction(db_);
  if (auto begun = transaction.Begin(); !begun) return std::unexpected(begun.error());
  if (changed) {
    auto clear = db_.Prepare("DELETE FROM catalog WHERE source = ?");
    if (!clear) return std::unexpected(clear.error());
    if (auto done = clear->Bind(1, source).Run(); !done) return std::unexpected(done.error());
    auto add = db_.Prepare(
        "INSERT INTO catalog(source, position, ref, title, play_seconds, owned, url) VALUES(?, ?, "
        "?, ?, ?, ?, ?)");
    if (!add) return std::unexpected(add.error());
    for (std::size_t at = 0; at < rows.size(); ++at) {
      const CatalogRow& row = rows[at];
      if (auto done = add->Bind(1, source)
                          .Bind(2, static_cast<std::int64_t>(at))
                          .Bind(3, row.ref)
                          .Bind(4, row.title)
                          .Bind(5, row.play_seconds)
                          .Bind(6, std::int64_t{row.owned ? 1 : 0})
                          .Bind(7, row.url)
                          .Run();
          !done) {
        return std::unexpected(done.error());
      }
    }
  }
  auto stamp = db_.Prepare(
      "INSERT INTO catalog_checked(source, checked_at) VALUES(?, ?) "
      "ON CONFLICT(source) DO UPDATE SET checked_at = excluded.checked_at");
  if (!stamp) return std::unexpected(stamp.error());
  if (auto done = stamp->Bind(1, source).Bind(2, model::NowSeconds()).Run(); !done)
    return std::unexpected(done.error());
  if (auto committed = transaction.Commit(); !committed) return std::unexpected(committed.error());
  return changed;
}

void MetadataStore::DropCatalog(const std::string& source) {
  std::lock_guard lock(mutex_);
  if (!db_.IsOpen()) return;
  for (const char* sql :
       {"DELETE FROM catalog WHERE source = ?", "DELETE FROM catalog_checked WHERE source = ?"}) {
    auto remove = db_.Prepare(sql);
    if (!remove) continue;
    if (auto done = remove->Bind(1, source).Run(); !done) {
      log::Warn("could not drop {}'s stored catalog: {}", source, done.error().message);
    }
  }
}

Result<void> MetadataStore::WriteLocked(const std::string& id, const json& info) {
  auto upsert = db_.Prepare(
      "INSERT INTO metadata(id, info, updated_at) VALUES(?, ?, ?) "
      "ON CONFLICT(id) DO UPDATE SET info = excluded.info, updated_at = excluded.updated_at");
  if (!upsert) return std::unexpected(upsert.error());
  const std::string text = info.dump(-1, ' ', false, json::error_handler_t::replace);
  if (auto done = upsert->Bind(1, id).Bind(2, text).Bind(3, model::NowSeconds()).Run(); !done)
    return done;

  auto clear = db_.Prepare("DELETE FROM artwork WHERE id = ?");
  if (!clear) return std::unexpected(clear.error());
  if (auto done = clear->Bind(1, id).Run(); !done) return done;
  json versions = json::object();
  for (const auto& [slot, key] : kSlots) {
    if (!info.contains(key)) continue;
    const std::string name = FileName(info[key]);
    if (name.empty()) continue;
    const std::string version = FileVersion(ArtworkDir(id) / name);
    if (version.empty()) continue;  // no file, no art
    auto add = db_.Prepare(
        "INSERT INTO artwork(id, slot, file, content_type, version) VALUES(?, ?, ?, ?, ?)");
    if (!add) return std::unexpected(add.error());
    const std::string content_type = info[key].value("content_type", std::string("image/jpeg"));
    if (auto done = add->Bind(1, id)
                        .Bind(2, slot)
                        .Bind(3, name)
                        .Bind(4, content_type)
                        .Bind(5, version)
                        .Run();
        !done) {
      return done;
    }
    if (InArtVersions(slot)) versions[slot] = version;
  }
  versions_[id] = std::move(versions);
  return {};
}

Result<void> MetadataStore::Write(const std::string& id, const json& info) {
  std::lock_guard lock(mutex_);
  if (!db_.IsOpen()) return Err("metadata_write_failed", "the metadata cache isn't open");
  Transaction transaction(db_);
  if (auto begun = transaction.Begin(); !begun) return begun;
  const json before = versions_.contains(id) ? versions_[id] : json();
  if (auto written = WriteLocked(id, info); !written) {
    if (before.is_null())
      versions_.erase(id);
    else
      versions_[id] = before;
    return written;
  }
  if (auto committed = transaction.Commit(); !committed) {
    if (before.is_null())
      versions_.erase(id);
    else
      versions_[id] = before;
    return committed;
  }
  const std::string tier = info.contains("protondb") && info["protondb"].is_object()
                               ? info["protondb"].value("tier", std::string())
                               : std::string();
  if (tier.empty())
    tiers_.erase(id);
  else
    tiers_[id] = tier;
  return {};
}

void MetadataStore::Remove(const std::string& id) {
  {
    std::lock_guard lock(mutex_);
    versions_.erase(id);
    tiers_.erase(id);
    DropGameRowsLocked(id);
    if (db_.IsOpen()) {
      if (auto remove = db_.Prepare("DELETE FROM metadata WHERE id = ?")) {
        if (auto done = remove->Bind(1, id).Run(); !done) {
          log::Warn("could not drop {}'s metadata: {}", id, done.error().message);
        }
      }
    }
  }
  std::error_code ec;
  fs::remove_all(ArtworkDir(id), ec);
}

void MetadataStore::KeepPicks(const std::string& id) {
  const json info = Read(id);
  json kept = json::object();
  std::set<std::string> files;
  for (const auto& [slot, key] : kSlots) {
    if (!info.contains(key) || !info[key].is_object() || !info[key].value("chosen", false))
      continue;
    kept[key] = info[key];
    if (const std::string name = FileName(info[key]); !name.empty()) files.insert(name);
  }
  if (kept.empty()) return Remove(id);
  {
    std::lock_guard lock(mutex_);
    DropGameRowsLocked(id);
  }
  if (auto written = Write(id, kept); !written) {
    log::Warn("could not trim {}'s metadata: {}", id, written.error().message);
    return;
  }
  std::error_code ec;
  std::vector<fs::path> dropped;
  for (const fs::directory_entry& entry : fs::directory_iterator(ArtworkDir(id), ec)) {
    if (!files.contains(entry.path().filename().string())) dropped.push_back(entry.path());
  }
  for (const fs::path& path : dropped) fs::remove_all(path, ec);
}

std::optional<MetadataStore::Art> MetadataStore::ArtFor(const std::string& id,
                                                        std::string_view slot) const {
  std::lock_guard lock(mutex_);
  if (!db_.IsOpen() || InfoKey(slot).empty()) return std::nullopt;
  auto select = db_.Prepare("SELECT file, content_type FROM artwork WHERE id = ? AND slot = ?");
  if (!select) return std::nullopt;
  select->Bind(1, id).Bind(2, slot);
  auto row = select->Step();
  if (!row || !*row) return std::nullopt;
  Art art{ArtworkDir(id) / select->Text(0), select->Text(1)};
  std::error_code ec;
  if (!fs::is_regular_file(art.file, ec)) return std::nullopt;
  return art;
}

json MetadataStore::ArtVersions(const std::string& id) const {
  std::lock_guard lock(mutex_);
  const auto found = versions_.find(id);
  return found == versions_.end() ? json::object() : found->second;
}

std::string MetadataStore::ProtonDbTier(const std::string& id) const {
  std::lock_guard lock(mutex_);
  const auto found = tiers_.find(id);
  return found == tiers_.end() ? std::string() : found->second;
}

std::vector<std::string> MetadataStore::PendingFits() const {
  std::lock_guard lock(mutex_);
  std::vector<std::string> ids;
  if (!db_.IsOpen()) return ids;
  auto select = db_.Prepare("SELECT id FROM fit_pending");
  if (!select) return ids;
  for (auto row = select->Step(); row && *row; row = select->Step()) ids.push_back(select->Text(0));
  return ids;
}

void MetadataStore::FitDone(const std::string& id) {
  std::lock_guard lock(mutex_);
  if (!db_.IsOpen()) return;
  if (auto remove = db_.Prepare("DELETE FROM fit_pending WHERE id = ?");
      remove && !remove->Bind(1, id).Run()) {
    log::Warn("could not mark {}'s art fitted", id);
  }
}

}  // namespace mira::store
