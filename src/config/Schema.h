#pragma once

#include <functional>
#include <json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mira::config {

// Global: one value for the whole daemon. PerGame: the same value, but a game
// can also override it (a game's overrides, PATCH /v1/games/{id}/config).
// GameOnly: only meaningful for a single game, so a settings screen lists it
// per game and not globally.
enum class Scope { Global, PerGame, GameOnly };

enum class Type { Bool, Int, Double, String, StringArray, Object };

// A String or StringArray whose values are paths on this computer, so a
// settings screen can offer a picker.
enum class PathKind { None, Folder, File };

using Validator = std::function<std::optional<std::string>(const nlohmann::json&)>;

// One value a StringArray's items are picked from, as a screen names it.
struct Choice {
  std::string value;
  std::string label;
  std::string doc;
  std::string pair = {};    // the value in the entry's paired_key that goes with this one
  bool needs_pair = false;  // only does anything while its pair is chosen
};

// A Validator plus the shape it enforces, so /v1/config/schema can publish
// that shape instead of a UI only learning it from a rejected PATCH.
// Constructible from a plain Validator when there's no shape to describe.
struct Constraint {
  Validator validate = {};
  std::vector<std::string> one_of;         // empty unless this is an enum
  std::vector<std::string> option_labels;  // what a screen calls each of one_of; may be empty
  std::vector<Choice> choices;    // empty unless this is a list whose items are picked from these
  std::optional<double> minimum;  // unset unless this is ranged
  std::optional<double> maximum;

  Constraint() = default;
  Constraint(Validator validator) : validate(std::move(validator)) {}  // NOLINT: implicit
  explicit operator bool() const { return static_cast<bool>(validate); }
};

// The games a per-game setting means anything for. Each list left empty matches every game.
struct Applies {
  std::vector<std::string> sources = {};         // the game's source is one of these
  std::vector<std::string> except_sources = {};  // and none of these
  std::vector<std::string> platforms = {};       // "windows", "native", "web"
  std::vector<std::string> runners = {};         // the kind in the game's runner_ref ("emulator")

  bool Matches(std::string_view source, std::string_view platform,
               std::string_view runner_ref) const;
  bool Empty() const {
    return sources.empty() && except_sources.empty() && platforms.empty() && runners.empty();
  }
};

// Declared with designated initializers in Schema.cpp, under the section it
// shows in.
struct Entry {
  std::string key;    // dotted, e.g. "scan.debounce_ms"
  std::string label;  // what a settings screen calls it
  Type type;
  nlohmann::json default_value;
  Scope scope = Scope::Global;
  std::string doc;
  std::string game_doc = {};   // help shown instead of `doc` in a game's own settings; optional
  Constraint constraint = {};  // optional

  bool is_secret = false;      // a credential: mask it
  bool is_runner_ref = false;  // a runner reference: offer a runner picker
  // The runner kinds that picker lists, without "auto"; empty lists every runner and "auto".
  std::vector<std::string> runner_kinds = {};
  std::string link = {};  // where to get the value, e.g. an API key page
  PathKind path = PathKind::None;
  std::string keywords = {};  // extra search terms a settings screen matches, e.g. "sgdb"
  // A key whose row this one is shown inside, e.g. a script's ready-made steps in its box.
  std::string shown_in = {};
  // A list whose choices pair with this one's (Choice::pair), e.g. a step and its undo.
  std::string paired_key = {};
  std::string unit = {};  // what a number counts, shown beside it: "s", "MB", "days"
  Applies applies = {};   // per-game settings only
  // Defaults it had before. Older versions wrote every default to settings.toml, so in a file from
  // then a stored one of these was never chosen, and is dropped when the file is cleaned.
  std::vector<nlohmann::json> old_defaults = {};

  // Set by the section the entry is declared under, never per entry.
  std::string category = {};
  int group = 0;                  // index within its category
  std::string group_label = {};   // the group's title, e.g. "Steam account"
  bool group_collapsed = false;   // a group of rarely changed values a screen shows folded
  bool group_resettable = false;  // a group whose values make sense to reset together
  // A few entries of a group that belong to one thing (an emulator, a download), which a screen
  // shows under one row named item_label; their labels make sense under it.
  std::string item = {};
  std::string item_label = {};
  // The source the key belongs to ("steam", "ubisoft"), derived from the key; empty if none.
  std::string source = {};
};

// Every configurable value in the daemon is declared here exactly once. The
// registry drives default materialisation, PATCH validation, the schema the
// frontend builds its settings UI from, and `mira config describe`. A new
// setting is one declaration and it exists in all four places.
class Schema {
 public:
  static const Schema& Instance();

  const std::vector<Entry>& Entries() const { return entries_; }
  const Entry* Find(std::string_view key) const;

  // Full default document, nested to match the on-disk layout.
  nlohmann::json Defaults() const;

  // Validates one value against its declared type and validator.
  std::optional<std::string> Validate(std::string_view key, const nlohmann::json& value) const;

  // "scan.debounce_ms" -> "/scan/debounce_ms"
  static nlohmann::json::json_pointer Pointer(std::string_view dotted_key);

 private:
  Schema();
  std::vector<Entry> entries_;
};

std::string_view ToString(Scope scope);
std::string_view ToString(Type type);

}  // namespace mira::config
