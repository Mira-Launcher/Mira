#include "store/Database.h"

#include <format>

#include <sqlite3.h>

namespace mira::store {

Statement::~Statement() { sqlite3_finalize(stmt_); }

Statement& Statement::Bind(int index, std::string_view text) {
  sqlite3_bind_text(stmt_, index, text.data(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
  return *this;
}

Statement& Statement::Bind(int index, std::int64_t value) {
  sqlite3_bind_int64(stmt_, index, value);
  return *this;
}

Statement& Statement::Bind(int index, double value) {
  sqlite3_bind_double(stmt_, index, value);
  return *this;
}

Statement& Statement::BindNull(int index) {
  sqlite3_bind_null(stmt_, index);
  return *this;
}

Result<bool> Statement::Step() {
  const int code = sqlite3_step(stmt_);
  if (code == SQLITE_ROW) return true;
  if (code == SQLITE_DONE) return false;
  return Err("database_error", std::format("database: {}", sqlite3_errmsg(sqlite3_db_handle(stmt_))), kDiskHint);
}

Result<void> Statement::Run() {
  auto stepped = Step();
  if (!stepped) return std::unexpected(stepped.error());
  return {};
}

std::string Statement::Text(int column) const {
  const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, column));
  return text ? std::string(text, sqlite3_column_bytes(stmt_, column)) : std::string();
}

std::int64_t Statement::Int(int column) const { return sqlite3_column_int64(stmt_, column); }
double Statement::Real(int column) const { return sqlite3_column_double(stmt_, column); }
bool Statement::IsNull(int column) const { return sqlite3_column_type(stmt_, column) == SQLITE_NULL; }

Database::~Database() { Close(); }

Result<void> Database::Open(const std::filesystem::path& file) {
  Close();
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  if (sqlite3_open_v2(file.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    const Error error = LastError(std::format("could not open {}", file.string()));
    Close();
    return std::unexpected(error);
  }
  sqlite3_busy_timeout(db_, 5000);
  return Exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;");
}

void Database::Close() {
  if (db_) sqlite3_close_v2(db_);
  db_ = nullptr;
}

Error Database::LastError(std::string_view what) const {
  return {"database_error", std::format("{}: {}", what, db_ ? sqlite3_errmsg(db_) : "out of memory"), kDiskHint};
}

Result<void> Database::Exec(std::string_view sql) {
  const std::string text(sql);
  if (sqlite3_exec(db_, text.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK) {
    return std::unexpected(LastError("database"));
  }
  return {};
}

Result<Statement> Database::Prepare(std::string_view sql) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr) != SQLITE_OK) {
    return std::unexpected(LastError("database"));
  }
  return Statement(stmt);
}

Result<void> Database::Migrate(std::span<const std::string_view> steps) {
  auto version = Prepare("PRAGMA user_version");
  if (!version) return std::unexpected(version.error());
  if (auto row = version->Step(); !row) return std::unexpected(row.error());
  const auto applied = static_cast<std::size_t>(version->Int(0));
  if (applied > steps.size()) {
    return Err("database_too_new",
               std::format("the library database is from a newer Mira (version {}, this one knows {})", applied,
                           steps.size()),
               "Update Mira, or start it with a copy of the library from before.");
  }
  for (std::size_t i = applied; i < steps.size(); ++i) {
    Transaction transaction(*this);
    if (auto begun = transaction.Begin(); !begun) return begun;
    if (auto done = Exec(steps[i]); !done) return done;
    if (auto done = Exec(std::format("PRAGMA user_version = {}", i + 1)); !done) return done;
    if (auto done = transaction.Commit(); !done) return done;
  }
  return {};
}

bool Database::Sound() {
  auto check = Prepare("PRAGMA quick_check");
  if (!check) return false;
  auto row = check->Step();
  return row && *row && check->Text(0) == "ok";
}

Result<void> Database::BackUp(const std::filesystem::path& file) {
  std::filesystem::path temp = file;
  temp += ".tmp";
  std::error_code ec;
  std::filesystem::remove(temp, ec);
  auto vacuum = Prepare("VACUUM INTO ?");
  if (!vacuum) return std::unexpected(vacuum.error());
  if (auto done = vacuum->Bind(1, temp.string()).Run(); !done) {
    std::filesystem::remove(temp, ec);
    return done;
  }
  std::filesystem::rename(temp, file, ec);
  if (ec) return Err("database_error", std::format("could not write {}: {}", file.string(), ec.message()), kDiskHint);
  return {};
}

Transaction::Transaction(Database& db) : db_(db) {}

Transaction::~Transaction() {
  if (open_) (void)db_.Exec("ROLLBACK");
}

Result<void> Transaction::Begin() {
  auto begun = db_.Exec("BEGIN IMMEDIATE");
  open_ = begun.has_value();
  return begun;
}

Result<void> Transaction::Commit() {
  auto committed = db_.Exec("COMMIT");
  if (committed) open_ = false;
  return committed;
}

}  // namespace mira::store
