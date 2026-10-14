#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "core/Result.h"

struct sqlite3;
struct sqlite3_stmt;

namespace mira::store {

// One prepared statement. Bind with 1-based indexes, Step until it returns false.
class Statement {
public:
  explicit Statement(sqlite3_stmt* stmt) : stmt_(stmt) {}
  Statement(Statement&& other) noexcept : stmt_(std::exchange(other.stmt_, nullptr)) {}
  Statement& operator=(Statement&&) = delete;
  Statement(const Statement&) = delete;
  ~Statement();

  Statement& Bind(int index, std::string_view text);
  Statement& Bind(int index, std::int64_t value);
  Statement& Bind(int index, double value);
  Statement& BindNull(int index);

  // True while there is a row to read.
  Result<bool> Step();
  Result<void> Run();  // Step for a statement that returns no rows; it can then be run again

  std::string Text(int column) const;
  std::int64_t Int(int column) const;
  double Real(int column) const;
  bool IsNull(int column) const;

private:
  // Keeps the first failed bind; Step reports it instead of running with a missing value.
  void NoteBind(int rc) {
    if (bind_rc_ == 0) bind_rc_ = rc;
  }

  sqlite3_stmt* stmt_;
  int bind_rc_ = 0;  // SQLITE_OK
};

class Database {
public:
  Database() = default;
  Database(const Database&) = delete;
  ~Database();

  Result<void> Open(const std::filesystem::path& file);
  void Close();
  bool IsOpen() const { return db_ != nullptr; }

  Result<void> Exec(std::string_view sql);
  Result<Statement> Prepare(std::string_view sql);
  // Applies the steps past PRAGMA user_version, each in a transaction of its own.
  Result<void> Migrate(std::span<const std::string_view> steps);
  // PRAGMA quick_check. "database_damaged" when it finds damage; any other error means the check
  // couldn't run (a busy or failing disk), which says nothing about the file.
  Result<void> CheckSound();
  // A consistent copy of the whole database, written beside `file` and renamed over it.
  Result<void> BackUp(const std::filesystem::path& file);

private:
  Error LastError(std::string_view what) const;

  sqlite3* db_ = nullptr;
};

// Rolls back unless committed. Not nestable: a caller already inside one doesn't open another.
class Transaction {
public:
  explicit Transaction(Database& db);
  ~Transaction();
  Transaction(const Transaction&) = delete;

  Result<void> Begin();
  Result<void> Commit();

private:
  Database& db_;
  bool open_ = false;
};

}  // namespace mira::store
