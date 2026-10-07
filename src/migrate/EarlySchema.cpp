#include "migrate/EarlySchema.h"

#include <format>
#include <string_view>

#include "core/Log.h"
#include "store/Database.h"

namespace mira::migrate {
namespace {

namespace fs = std::filesystem;

// Whether the query finds a row: a table or a column that exists.
bool Found(store::Database& db, std::string_view sql) {
  auto query = db.Prepare(sql);
  if (!query) return false;
  auto row = query->Step();
  return row && *row;
}

void Run(store::Database& db, std::string_view sql, const fs::path& file) {
  if (auto done = db.Exec(sql); !done) {
    log::Warn("could not bring {} up to date: {}", file.string(), done.error().message);
  }
}

}  // namespace

void RepairEarlySchemas(const fs::path& library_db, const fs::path& cache_db) {
  std::error_code ec;
  if (fs::exists(library_db, ec)) {
    store::Database db;
    if (db.Open(library_db) && Found(db, "SELECT 1 FROM sqlite_master WHERE name = 'games'")) {
      for (const char* column : {"library_link", "folder_tag"}) {
        if (Found(db, std::format("SELECT 1 FROM pragma_table_info('games') WHERE name = '{}'",
                                  column)))
          continue;
        Run(db, std::format("ALTER TABLE games ADD COLUMN {} TEXT NOT NULL DEFAULT ''", column),
            library_db);
      }
    }
  }
  if (fs::exists(cache_db, ec)) {
    store::Database db;
    if (db.Open(cache_db) && Found(db, "SELECT 1 FROM sqlite_master WHERE name = 'metadata'") &&
        !Found(db, "SELECT 1 FROM sqlite_master WHERE name = 'lists'")) {
      Run(db,
          "CREATE TABLE lists(name TEXT PRIMARY KEY, value TEXT NOT NULL CHECK(json_valid(value)), "
          "updated_at INTEGER NOT NULL) STRICT",
          cache_db);
    }
  }
}

}  // namespace mira::migrate
