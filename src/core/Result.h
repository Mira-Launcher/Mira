#pragma once

#include <expected>
#include <string>
#include <utility>

namespace mira {

// Bumped on a breaking change to the REST API; GET /v1/health reports it so a newer client can spot an older daemon.
inline constexpr int kApiVersion = 2;

// Where the user can fix an error, for clients to turn into a button or a
// command; empty `kind` for none. Kinds and fields: docs/api.md, "Errors".
struct Fix {
  std::string kind;
  std::string target;
  std::string step;

  static Fix Setting(std::string key) { return {"setting", std::move(key), {}}; }
  static Fix Runners(std::string tool = {}) { return {"runners", std::move(tool), {}}; }
  static Fix Source(std::string id, std::string step) { return {"source", std::move(id), std::move(step)}; }
  static Fix Game(std::string id, std::string step) { return {"game", std::move(id), std::move(step)}; }
};

// Errors cross every interface boundary as values, never as exceptions: a
// runner implementation that throws must not be able to take down the daemon.
struct Error {
  std::string code;     // stable, machine-readable, e.g. "runner_missing"
  std::string message;  // human-readable: what went wrong, for any client
  std::string hint;     // optional: what to do about it, in words any client can show
  Fix fix = {};         // optional: where to do it
};

template <typename T>
using Result = std::expected<T, Error>;

// The hint for a download or web API call that didn't get through.
inline constexpr const char* kConnectionHint =
    "Check the internet connection and try again. GitHub limits how often it answers, so waiting a few "
    "minutes can help.";

// The hint for a file Mira couldn't write.
inline constexpr const char* kDiskHint = "Check the disk isn't full and the folder is writable.";

inline std::unexpected<Error> Err(std::string code, std::string message, std::string hint = {},
                                  Fix fix = {}) {
  return std::unexpected(Error{std::move(code), std::move(message), std::move(hint), std::move(fix)});
}

}  // namespace mira
