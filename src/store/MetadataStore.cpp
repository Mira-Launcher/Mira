#include "store/MetadataStore.h"

#include <array>
#include <format>
#include <fstream>
#include <utility>

#include "core/Log.h"
#include "model/Types.h"

namespace mira::store {
namespace {
using nlohmann::json;
namespace fs = std::filesystem;

constexpr std::array<std::string_view, 1> kMigrations = {
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
};

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

bool InArtVersions(std::string_view slot) { return slot != "capsule" && slot != "header"; }

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
  return std::format("{:x}-{:x}", static_cast<std::uint64_t>(written.time_since_epoch().count()), size);
}

std::string FileName(const json& slot) {
  const std::string name = slot.is_object() ? slot.value("file", std::string()) : std::string();
  // Only a plain name inside the id's folder.
  return name.find('/') == std::string::npos && name != ".." ? name : std::string();
}
}  // namespace

MetadataStore::MetadataStore(fs::path dir) : dir_(std::move(dir)) {}

fs::path MetadataStore::ArtworkDir(const std::string& id) const { return dir_ / "artwork" / id; }

Result<void> MetadataStore::Open() {
  const fs::path file = dir_ / "cache.db";
  auto opened = db_.Open(file);
  if (opened && !db_.Sound()) opened = Err("database_damaged", "the cache failed its check");
  if (!opened) {
    // Everything in it can be fetched again.
    log::Warn("the metadata cache {} is damaged ({}); starting it over", file.string(), opened.error().message);
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
    out[select->Text(0)] = select->IsNull(1) ? json() : json::parse(select->Text(1), nullptr, false);
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

Result<void> MetadataStore::WriteLocked(const std::string& id, const json& info) {
  auto upsert = db_.Prepare(
      "INSERT INTO metadata(id, info, updated_at) VALUES(?, ?, ?) "
      "ON CONFLICT(id) DO UPDATE SET info = excluded.info, updated_at = excluded.updated_at");
  if (!upsert) return std::unexpected(upsert.error());
  const std::string text = info.dump(-1, ' ', false, json::error_handler_t::replace);
  if (auto done = upsert->Bind(1, id).Bind(2, text).Bind(3, model::NowSeconds()).Run(); !done) return done;

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
    auto add = db_.Prepare("INSERT INTO artwork(id, slot, file, content_type, version) VALUES(?, ?, ?, ?, ?)");
    if (!add) return std::unexpected(add.error());
    const std::string content_type = info[key].value("content_type", std::string("image/jpeg"));
    if (auto done = add->Bind(1, id).Bind(2, slot).Bind(3, name).Bind(4, content_type).Bind(5, version).Run(); !done) {
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
    if (before.is_null()) versions_.erase(id); else versions_[id] = before;
    return written;
  }
  if (auto committed = transaction.Commit(); !committed) {
    if (before.is_null()) versions_.erase(id); else versions_[id] = before;
    return committed;
  }
  return {};
}

void MetadataStore::Remove(const std::string& id) {
  {
    std::lock_guard lock(mutex_);
    versions_.erase(id);
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

std::optional<MetadataStore::Art> MetadataStore::ArtFor(const std::string& id, std::string_view slot) const {
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

}  // namespace mira::store
