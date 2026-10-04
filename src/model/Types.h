#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <json.hpp>

namespace mira::model {

enum class Platform { Unknown, Windows, Native };

enum class GameStatus {
  SettingUp,   // detected and configured; its data directory is being provisioned
  Ready,
  Broken,      // provisioning or launching failed; last_error says what to do
  Missing,     // the folder disappeared, but the configuration is kept
  NeedsInstall, // the only thing found was an installer, not the game itself
};

std::string_view ToString(Platform platform);
std::string_view ToString(GameStatus status);
Platform PlatformFromString(std::string_view text);
GameStatus GameStatusFromString(std::string_view text);

// Severity of a `notification` event (EventBus::PublishNotification), the
// frontend maps this straight onto its toast/system-notification styling.
enum class NotifyLevel { Info, Success, Warning, Error };
std::string_view ToString(NotifyLevel level);

// One executable the detector found, offered to the frontend as an
// alternative to the one it chose. Identified by rel_path, not a numeric id:
// there is no database assigning one.
struct Candidate {
  std::string rel_path;
  Platform kind = Platform::Unknown;
  double score = 0.0;
  bool chosen = false;
  bool is_installer = false;  // name + size say this is a setup.exe, not the game
};

// A game's id is a filesystem-safe slug derived from its name (e.g.
// "celeste"), not an opaque integer: games.toml is meant to be readable and
// hand-editable, and a slug reads naturally as a TOML table key's value.
struct Game {
  std::string id;
  std::string install_path;   // parent_path() is which library root this came from
  std::string name;
  GameStatus status = GameStatus::SettingUp;
  double confidence = 0.0;
  bool reviewed = false;
  Platform platform = Platform::Unknown;
  // Who found this game: "scan", "steam", or "lutris". Lets a frontend warn
  // when exe_path isn't what actually launches it (true for "steam").
  std::string source = "scan";
  std::string exe_path;      // relative to install_path
  std::string args;
  std::string working_dir;   // relative to install_path; empty means the exe's directory

  // "kind:name", e.g. "proton:GE-Proton11-7"; empty until resolved.
  std::string runner_ref;

  // Opaque, owned by whatever `source` is, e.g. Legendary's app_name for
  // an "epic" game. Distinct from runner_config, which is owned by the
  // runner instead. Empty for sources that don't need one.
  std::string source_ref;

  // The game's private directory. A Wine/Proton prefix today, but core does
  // not know that: what lives inside belongs to the runner.
  std::string data_dir;

  // Where the game's installer was, once it installed the game somewhere else; a scan
  // treats that folder as this game's, not a new one. Empty otherwise.
  std::string installer_dir;

  nlohmann::json runner_config = nlohmann::json::object();  // opaque, owned by the runner
  nlohmann::json overrides = nlohmann::json::object();      // dotted config keys

  std::string last_error;
  std::int64_t created_at = 0;
  std::int64_t updated_at = 0;
  std::optional<std::int64_t> last_played_at;
  std::int64_t play_seconds = 0;

  std::map<std::string, std::string> env;
  std::vector<Candidate> candidates;

  // Free-form, user-assigned. No tag is special-cased in storage: "hidden"
  // is a convention the API/CLI treat specially (excluded from the default
  // GET /v1/games list; see docs/api.md), not a separate field.
  std::vector<std::string> tags;
};

// An installed runner build, e.g. GE-Proton11-7. Rediscovered at startup and
// on demand rather than persisted, so it has no stored id, so a game refers to
// one by its "kind:name" reference string.
struct RunnerBuild {
  std::string kind;  // "native", "wine", "proton", ...
  std::string name;
  std::string path;
  std::string version;

  std::string Reference() const { return kind + ":" + name; }
};

struct Event {
  std::int64_t id = 0;
  std::int64_t ts = 0;
  std::string type;
  nlohmann::json payload;
};

std::int64_t NowSeconds();

nlohmann::json ToJson(const Candidate& candidate);
nlohmann::json ToJson(const Game& game);
nlohmann::json ToJson(const RunnerBuild& runner);
nlohmann::json ToJson(const Event& event);

// Inverse of ToJson(const Game&), used when loading games.toml. Missing or
// malformed fields fall back to their model defaults rather than failing the
// whole load, so one bad entry cannot take down the rest of the library.
Game GameFromJson(const nlohmann::json& document);

}  // namespace mira::model
