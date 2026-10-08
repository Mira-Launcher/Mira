#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ApiError.h"

// The plain data mirad's REST API speaks, as C++ structs.
//
// Deliberately free of Qt and of httplib: these are what the endpoints in
// client/api/ return and what every widget in the frontend reads, so they
// are the one part of the client layer the UI is allowed to depend on
// directly. See docs/api.md for the JSON each one mirrors.
namespace mira_gui {

// A game's `art`: slot -> version of its cached image (docs/api.md).
using ArtVersions = std::map<std::string, std::string>;

// Matches mira::kApiVersion in src/core/Result.h.
inline constexpr int kExpectedApiVersion = 1;

struct HealthStatus {
  bool reachable = false;
  int api = 0;  // 0 when mirad predates the field
  std::string detail;
};

// Mirrors the subset of the `Game` fields (docs/api.md, GET /v1/games) a
// library list needs, not the full record.
struct GameSummary {
  std::string id;
  std::string name;
  std::string status;
  std::string platform;
  std::string runner_ref;
  std::string last_error;
  std::string install_path;
  // mirad picked the executable itself, wasn't sure, and nobody has confirmed it.
  bool needs_check = false;
  std::optional<std::int64_t> last_played_at;
  std::int64_t play_seconds = 0;
  // Free-form, user-assigned (docs/api.md). "hidden" is the one convention
  // the frontend treats specially: excluded from the library by default,
  // shown only by the Hidden filter (Ctrl+H).
  std::vector<std::string> tags;
  // Which importer owns it: "scan", "steam", "epic", "lutris", "battlenet",
  // ... ("launcher" for a store launcher's own install).
  std::string source;
  bool running = false;
  // Slot ("cover", "hero", ...) -> version, only for slots with an image.
  // Unset when the record didn't say, which means nothing is known either way.
  std::optional<ArtVersions> art;
  // The library folder (as library_roots spells it) whose folders this game is sorted into by
  // tag; empty when its folder never moves. `folder_tags` are that folder's folder tags, unset
  // while it isn't sorted (docs/api.md, Folders by tag).
  std::string sort_root;
  std::optional<std::vector<std::string>> folder_tags;
  // The folder tag picked for this game over the folder tags' order; empty to follow that order.
  std::string folder_tag;

  bool operator==(const GameSummary&) const = default;
};

// The tags the frontend gives a meaning to.
namespace tags {
// Pinned to the sidebar. "favorite" because Lutris imports its favorites under it.
inline constexpr const char* kPinned = "favorite";
inline constexpr const char* kHidden = "hidden";
// A program rather than a game: kept out of Continue, with no playtime shown.
inline constexpr const char* kApp = "app";
}  // namespace tags

inline bool HasTag(const GameSummary& game, std::string_view tag) {
  return std::ranges::find(game.tags, tag) != game.tags.end();
}
inline bool IsApp(const GameSummary& game) { return HasTag(game, tags::kApp); }
inline bool IsHidden(const GameSummary& game) { return HasTag(game, tags::kHidden); }
inline bool IsMeaningTag(std::string_view tag) {
  return tag == tags::kPinned || tag == tags::kHidden || tag == tags::kApp;
}

// Tags compared as mirad compares folder tags with them: ignoring ASCII case.
inline bool SameTag(std::string_view a, std::string_view b) {
  return std::ranges::equal(a, b, [](char x, char y) {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    return lower(x) == lower(y);
  });
}

// Which of `tags` is the game's folder tag, or -1, as mirad picks it: `pick` while the game has it
// and it's one of `folder_tags`, else the first of `folder_tags` (in their order) the game has.
// The tags Mira gives a meaning to never are.
inline int FolderTagIndex(const std::vector<std::string>& tags,
                          const std::optional<std::vector<std::string>>& folder_tags,
                          std::string_view pick = {}) {
  if (!folder_tags) return -1;
  const auto index_of = [&](std::string_view tag) {
    for (size_t i = 0; i < tags.size(); ++i) {
      if (!IsMeaningTag(tags[i]) && SameTag(tags[i], tag)) return static_cast<int>(i);
    }
    return -1;
  };
  if (!pick.empty() &&
      std::ranges::any_of(*folder_tags, [&](const std::string& f) { return SameTag(f, pick); })) {
    if (const int at = index_of(pick); at >= 0) return at;
  }
  for (const std::string& folder : *folder_tags) {
    if (const int at = index_of(folder); at >= 0) return at;
  }
  return -1;
}

// A game's `tags` as `order` (the library's TagOrder) lists them; tags it doesn't list keep their
// own order after those, and the tags Mira gives a meaning to come last.
inline std::vector<std::string> InTagOrder(std::vector<std::string> tags, const std::vector<std::string>& order) {
  const auto rank = [&](const std::string& tag) {
    if (IsMeaningTag(tag)) return order.size() + 1;
    const auto at = std::ranges::find_if(order, [&](const std::string& o) { return SameTag(o, tag); });
    return static_cast<std::size_t>(at - order.begin());
  };
  std::ranges::stable_sort(tags, {}, rank);
  return tags;
}
// An app is opened and runs; a game is played.
inline const char* RunVerb(const GameSummary& game) { return IsApp(game) ? "Open" : "Play"; }
inline const char* RunningLabel(const GameSummary& game) { return IsApp(game) ? "Running" : "Playing"; }
inline bool IsPinned(const GameSummary& game) { return HasTag(game, tags::kPinned); }

// Play or Stop does something: it runs, or it's ready to launch. Anything else would only get a 409.
inline bool CanPlayOrStop(const GameSummary& game) { return game.running || game.status == "ready"; }

struct GamesResult {
  bool ok = false;
  ApiError error;
  std::vector<GameSummary> games;
};

// docs/api.md's `game.state` event, trimmed to what a row's Launch/Stop
// button needs; see ParseGameState.
struct GameStateEvent {
  std::string id;
  std::string state;  // "running" | "exited" | "crashed"
  // After an exit: how long it ran, and for a crash what went wrong and what to do.
  std::int64_t played_seconds = 0;
  ApiError error;
};

struct DeleteResult {
  bool ok = false;
  ApiError error;
};

// POST /v1/games/{id}/launch. The actual outcome (running, exited,
// crashed) arrives later as a `game.state` SSE event (docs/api.md), since
// launch returns as soon as the process exists, not when it finishes.
//
// `tracked` is mirad's own answer to "are game.state events coming for this
// launch" (docs/api.md), not something inferred here. It is false only for
// a Steam-sourced game under `steam.launch_mode: "steam"` *with*
// `steam.track_process` off: mirad handed it to
// `steam://rungameid/<appid>` and is watching nothing. With track_process
// on (the default) mirad polls /proc for it and real game.state events do
// arrive, a few seconds later than a normal launch.
//
// A caller that assumes tracking when there is none marks the game as
// playing forever, because nothing will ever say it stopped; a caller that
// assumes none when there is marks it stopped while it runs.
struct LaunchResult {
  bool ok = false;
  ApiError error;
  bool tracked = true;
};

// `game.launched`, the event mirad publishes for a Steam launch, carrying
// the same `tracked` the launch reply does. Needed as an event and not just
// a reply because the launch may have come from somewhere else entirely
// (the CLI, the other window), and then this is all a client ever sees.
struct GameLaunchedEvent {
  std::string id;
  bool tracked = false;
};

// game.install_detected: a launched game exited and had added a program
// folder to its prefix, so it was an installer.
struct InstallDetectedEvent {
  std::string id;
  std::string install_path;  // absolute, inside the game's prefix
  std::string exe_path;      // relative to install_path; empty when none was found
};

// game.installer_leftover: an install left the game away from its installer's folder.
struct InstallerLeftoverEvent {
  std::string id;
  std::string installer_dir;
  std::int64_t bytes = 0;
};

// GET /v1/games/{id}/artwork: the cached cover image itself, as bytes.
//
// `missing` is the 404 case and is not an error: most games have no cached
// artwork, and the placeholder cover is the intended answer for them. Only
// a reachability or server failure sets `error`.
struct ArtworkResult {
  bool ok = false;
  bool missing = false;
  ApiError error;
  std::string bytes;
  std::string content_type;
};

// GET /v1/games/{id}/metadata: the store info mirad cached alongside the
// art. Only what a details panel shows; the cached JSON carries more
// (screenshots, requirements, DLC ids) than anything here reads.
// One entry from art_candidates (docs/api.md, GET .../metadata), a
// SteamGridDB result not necessarily the one currently applied. `url`/
// `thumb` are left out: they're addresses on SteamGridDB's own CDN, and the
// frontend has no HTTP client for the open internet, only mirad's socket.
// A candidate is chosen by `id` and mirad fetches it, never the frontend.
struct ArtCandidate {
  std::int64_t id = 0;
  int width = 0;
  int height = 0;
  std::string style;
  std::string source;  // "steamgriddb", "steam_cdn", "epic", "lutris"
  bool nsfw = false;   // SteamGridDB marks it adult
};

// game.artwork_thumbs_ready: which previews one POST .../artwork/thumbs batch
// left on disk. `error` only when the whole batch failed.
struct ArtThumbsEvent {
  std::string id;
  std::string slot;
  std::vector<std::int64_t> ready;
  std::vector<std::int64_t> failed;
  ApiError error;
};

// game.artwork_candidates_ready: one page of SteamGridDB's art for a slot
// (POST .../artwork/candidates). `code`/`error` when it couldn't be fetched.
struct ArtCandidatesEvent {
  std::string id;
  std::string slot;
  int page = 0;
  int total = 0;
  std::string request;  // as passed to FetchArtCandidatesAsync
  std::vector<ArtCandidate> candidates;
  std::string code;
  ApiError error;
};

// GET .../artwork/thumb for each id of a batch; an id without a cached
// preview is left out.
struct ArtThumbsResult {
  std::vector<std::pair<std::int64_t, std::string>> images;  // id, bytes
};

struct GameMetadata {
  std::string source;  // "steam" | "steamgriddb"
  std::string description;
  std::string release_date;
  std::vector<std::string> developers;
  std::vector<std::string> genres;
  std::string price;
  int metacritic_score = 0;
  std::string review_summary;  // "Very Positive", from Steam's own wording
  int review_total = 0;
  std::string protondb_tier;
  std::vector<std::string> steam_tags;  // most voted first
  std::string website;
  // Which art slots are actually cached, so a panel knows whether asking for
  // one is worth a round trip. See GET /v1/games/{id}/artwork?type=.
  std::vector<std::string> art_slots;
  // Every cover/hero SteamGridDB returned, cached alongside whichever one is
  // active. Populated for a Steam-owned game too now (as alternates to
  // Steam's own CDN default, not a replacement for it). Empty only when no
  // steamgriddb.api_key is set, or SteamGridDB has no match for the name.
  std::vector<ArtCandidate> cover_candidates;
  std::vector<ArtCandidate> hero_candidates;
  // The candidate currently applied to each slot, when it came from one of
  // the lists above, and unset for Steam's own CDN art, which isn't a
  // candidate. What ArtworkPickerDialog marks "(current)".
  std::optional<std::int64_t> cover_active_candidate_id;
  std::optional<std::int64_t> hero_active_candidate_id;

  // The wider store info that doesn't fit the sidebar; see
  // GameDetailPageDialog. requirements_min/rec are HTML, not plain text.
  std::string requirements_min;
  std::string requirements_rec;
  std::vector<std::int64_t> dlc_ids;
  std::vector<std::string> content_descriptors;
  int achievements_total = 0;
  std::vector<std::string> screenshots;  // URLs, opened externally
  std::vector<std::string> trailers;     // mp4 URLs, opened externally
};

struct GameMetadataResult {
  bool ok = false;
  bool missing = false;  // never fetched, or fetched and found nothing
  ApiError error;
  GameMetadata metadata;
};

// POST /v1/games/{id}/metadata/refresh: 202, so this says only that the
// fetch was accepted. The outcome arrives as game.metadata_ready or
// game.metadata_failed on the event stream (docs/api.md).
struct MetadataRefreshResult {
  bool ok = false;
  ApiError error;
};

// POST /v1/games/metadata/refresh and .../refresh-missing, jobs that finish
// once every game's fetch has.
struct MetadataBatchResult {
  bool ok = false;
  ApiError error;
  int refreshed = 0;
  int failed = 0;
};

// A game.metadata_ready / game.metadata_failed payload, trimmed to what the
// UI acts on.
struct MetadataEvent {
  std::string id;
  // Both only on .metadata_failed. Branch on `code`, never on `error`:
  // `error` is a sentence written for a human to read.
  std::string code;
  ApiError error;
  std::optional<ArtVersions> art;  // the game's art after the fetch
};

// game.artwork_selected / .artwork_select_failed: the outcome of
// POST /v1/games/{id}/artwork?type=, which itself only returns 202.
struct ArtworkSelectEvent {
  std::string id;
  std::string slot;
  ApiError error;                  // .artwork_select_failed only
  std::optional<ArtVersions> art;  // .artwork_selected only
};

// POST /v1/games/{id}/artwork?type=: 202, so this is only "accepted", not
// "done". The outcome is ArtworkSelectEvent on the event stream.
struct ArtworkSelectResult {
  bool ok = false;
  ApiError error;
};

// A `notification` event: mirad's own decision that this is worth telling
// the user about; the UI just renders it (see events::ParseNotification
// and mira_gui::notify::Warn/Notice).
struct NotificationEvent {
  std::string level;  // "info" | "success" | "warning" | "error"
  std::string message;
};

struct StopResult {
  bool ok = false;
  ApiError error;
};

struct ScanResult {
  bool ok = false;
  ApiError error;
  int added = 0;
  int missing = 0;
  int restored = 0;
};

// The full record GET /v1/games/{id} returns (docs/api.md), everything a
// detail/edit view needs, beyond GameSummary's list-row subset.
struct GameDetail {
  struct Candidate {
    std::string rel_path;
    std::string kind;
    double score = 0.0;
    bool chosen = false;
    bool is_installer = false;  // name + size say this is a setup.exe, not the game
  };

  std::string id;
  std::string name;
  std::string status;
  std::string platform;
  // "scan", "steam", "lutris", or "desktop-entry", which source owns this
  // game's own fields on a rescan/re-import. GameEditForm uses it to warn
  // when exe_path isn't actually what launches the game; DeleteGameDialog
  // uses "desktop-entry" to disable file/prefix deletion.
  std::string source;
  std::string install_path;
  std::string exe_path;
  std::string args;
  std::string working_dir;
  std::string runner_ref;
  std::string data_dir;
  std::string last_error;
  bool needs_check = false;  // as GameSummary's
  std::optional<std::int64_t> last_played_at;
  std::int64_t play_seconds = 0;
  // `runner_config`/`env` as JSON object text; GameEditForm edits them as rows
  // and sends only the keys that changed.
  std::string runner_config_json;
  std::string env_json;
  // What an empty runner_ref runs with ("proton:auto"), from GET /v1/games/{id} only.
  std::string default_runner;
  std::vector<Candidate> candidates;
  std::vector<std::string> tags;
  std::string sort_root;  // as GameSummary's
  std::optional<std::vector<std::string>> folder_tags;
  std::string folder_tag;
};

struct GameDetailResult {
  bool ok = false;
  ApiError error;
  GameDetail game;
};

// A subset of PATCH /v1/games/{id}'s body (docs/api.md): only the fields
// present are sent, matching the endpoint's own "any subset" contract.
struct GamePatch {
  std::optional<std::string> name;
  std::optional<std::string> exe_path;
  std::optional<std::string> args;
  std::optional<std::string> working_dir;
  std::optional<std::string> runner_ref;
  std::optional<std::string> data_dir;
  // Raw JSON text (must parse to an object); see GameDetail's comment.
  std::optional<std::string> runner_config_json;
  std::optional<std::string> env_json;
  // Replaces the whole set (docs/api.md); there's no per-entry merge for a
  // plain list the way env's null-removes-a-key convention gives it one.
  std::optional<std::vector<std::string>> tags;
  // true confirms mirad's pick of executable without changing anything else.
  std::optional<bool> reviewed;
};

struct PatchGameResult {
  bool ok = false;
  ApiError error;
};

// One GET /v1/config/schema entry (docs/api.md): every backend setting
// declared exactly once in src/config/Schema.cpp, which is what lets a
// settings screen exist with zero hardcoded knowledge of what settings
// there are. `type` is one of the human strings config::ToString(Type)
// produces: "a boolean" | "an integer" | "a number" | "a string" |
// "an array of strings" | "an object". Entries arrive in display order.
struct ConfigSchemaEntry {
  std::string key;
  std::string label;  // display name; the key is only what's stored
  std::string type;
  std::string doc;
  std::string game_doc;  // help for a game's own settings; empty means `doc`
  std::string default_display;

  // What the daemon will accept, when that has a shape worth rendering
  // (docs/api.md). Both are absent unless they apply, so a non-empty
  // `one_of` means "this is an enum, offer exactly these" and a set
  // `minimum` means "this is bounded, clamp the spin box to it". Without
  // them every setting is a free-text box and the rules only surface as a
  // rejection after saving.
  std::vector<std::string> one_of;
  std::optional<double> minimum;
  std::optional<double> maximum;
  std::string category;        // UI grouping; always present
  int group = 0;               // settings sharing this number within a category form one card
  std::string group_label;     // that card's title
  bool group_collapsed = false;  // the card starts folded
  bool group_resettable = false;  // the card offers one reset to its defaults
  std::string source;          // e.g. "steam" when the setting belongs to one source; else empty
  std::string path;            // "folder" or "file" when the value is a path on this computer
  bool per_game = false;       // overridable per game (scope "per_game" or "game_only")
  bool game_only = false;      // only exists per game; not listed in the global settings
  bool is_secret = false;      // mask this value's field
  bool is_runner_ref = false;  // offer a runner picker (GET /v1/runners) instead of free text
  std::string link;            // web page where the user gets the value; empty if none
  std::string keywords;        // extra search terms, space-separated
};

struct ConfigSchemaResult {
  bool ok = false;
  ApiError error;
  std::vector<ConfigSchemaEntry> entries;
};

struct ConfigResult {
  bool ok = false;
  ApiError error;
  // Every leaf of GET /v1/config's document, flattened to dotted keys
  // matching Schema entries' own `key` (e.g. "scan.debounce_ms"), each
  // stringified for display/editing: a bool as "true"/"false", a number in
  // its natural text form, an array as JSON (mapping::ParseListText). The opaque
  // `frontend` table (docs/architecture.md) is excluded because it isn't part of
  // the schema this screen renders.
  std::map<std::string, std::string> values;
};

// One edit to send in a PATCH /v1/config body. `type` is the schema type
// string (ConfigSchemaEntry::type), so the string typed into the UI can be
// converted back to the right JSON kind before sending.
struct ConfigEdit {
  std::string key;
  std::string type;
  std::string value;
};

struct PatchConfigResult {
  bool ok = false;
  ApiError error;
};

// One entry from GET /v1/runners (docs/api.md): an installed build of one
// runner kind, discovered fresh on every call. `reference` is what a game's
// `runner_ref` field and the `default_runner.*` settings both use, the
// only string that actually round-trips, `name`/`version` are just for
// display.
struct RunnerInfo {
  std::string kind;
  std::string name;
  std::string version;
  std::string reference;
  std::string path;
  std::string label;       // readable name
  std::string source;      // the download source it came from, if known
  bool removable = false;  // false for distro, Steam and system builds
};

struct RunnersResult {
  bool ok = false;
  ApiError error;
  std::vector<RunnerInfo> runners;
};

// One key from GET /v1/games/{id}/config (docs/api.md): every schema key
// resolved through default -> settings.toml -> this game's overrides,
// tagged with which layer supplied it. Only entries with `overridable: true`
// (config::Resolver::IsOverridable) make sense to show as editable, since some
// settings describe the daemon rather than a game (e.g. `library_roots`)
// and are excluded there for exactly that reason.
struct GameConfigEntry {
  std::string key;
  std::string value_display;  // stringified like ConfigResult::values
  std::string layer;          // "default" | "config" | "game"
  bool overridable = false;
};

struct GameConfigResult {
  bool ok = false;
  ApiError error;
  std::vector<GameConfigEntry> entries;
};

// One edit to send in a PATCH /v1/games/{id}/config body. `clear` sends a
// JSON null for `key`, removing this game's override and falling back to
// the next layer down, the per-game equivalent of ConfigEdit, which has no
// such concept since a global setting has no further layer to fall back to.
struct GameConfigEdit {
  std::string key;
  std::string type;
  std::string value;
  bool clear = false;
};

struct PatchGameConfigResult {
  bool ok = false;
  ApiError error;
};

// PATCH /v1/games: one request for many games.
struct GamesPatch {
  std::vector<std::string> ids;
  std::vector<std::string> add_tags;
  std::vector<std::string> remove_tags;
  // Each game's folder from now on (added if missing), whatever the folder tags' order says; ""
  // goes back to that order.
  std::optional<std::string> folder_tag;
  std::vector<GameConfigEdit> config;
};

// One of the library's tags (GET /v1/tags): the games that have it, whether it's a folder tag, and
// the games Steam gives it.
struct TagSummary {
  std::string name;
  std::vector<std::string> ids;
  bool folder = false;
  std::vector<std::string> steam_ids;
};

// A Steam tag on the library's games that isn't one of its tags yet.
struct SteamTagSuggestion {
  std::string name;
  std::vector<std::string> ids;
};

struct TagsResult {
  bool ok = false;
  ApiError error;
  std::vector<TagSummary> tags;
  std::vector<SteamTagSuggestion> steam;
  int steam_missing = 0;  // games whose Steam tags were never fetched
};

struct SteamTagsFetchResult {
  bool ok = false;
  ApiError error;
  int fetched = 0;
};

// A game that would move if the folder tags or sorted folders changed (POST /v1/tags/preview).
struct FolderTagsMove {
  std::string id;
  std::string name;
  std::string to;
};

struct FolderTagsPreviewResult {
  bool ok = false;
  ApiError error;
  std::vector<FolderTagsMove> moving;
};

// A folder that could be any of several games moved by hand (GET /v1/library/unclear).
struct UnclearMove {
  std::string folder;
  std::vector<std::pair<std::string, std::string>> games;  // id, name
};

struct UnclearMovesResult {
  bool ok = false;
  ApiError error;
  std::vector<UnclearMove> moves;
};

struct SettleMoveResult {
  bool ok = false;
  ApiError error;
  GameSummary game;
};

struct PatchGamesResult {
  bool ok = false;
  ApiError error;
  std::vector<GameSummary> games;  // only the ones that changed
};

// One release from GET /v1/runners/catalog (docs/api.md): a runner build
// that is *available to install*, as opposed to RunnerInfo, which is one
// already installed. `tag` is what POST /v1/runners/download takes.
struct RunnerRelease {
  std::string tag;
  std::string name;    // what download events call it
  std::string label;   // readable name
  std::string source;  // GET /v1/runners/sources id
  bool installed = false;
  std::string asset_name;
  std::int64_t size_bytes = 0;
  std::string published_at;
  bool has_checksum = false;
};

struct RunnerCatalogResult {
  bool ok = false;
  ApiError error;
  std::vector<RunnerRelease> releases;
};

// GET /v1/runners/sources: where builds of a kind download from.
struct RunnerSourceInfo {
  std::string id;
  std::string label;
};

struct RunnerSourcesResult {
  bool ok = false;
  ApiError error;
  std::vector<RunnerSourceInfo> sources;
};

// GET /v1/runners/updates: an installed build with a newer release.
struct RunnerUpdate {
  std::string reference;
  std::string source;
  std::string tag;
  std::string name;  // the newer release's
  std::string label;
};

struct RunnerUpdatesResult {
  bool ok = false;
  ApiError error;
  std::vector<RunnerUpdate> updates;
};

// GET /v1/runners/tools: umu-launcher and winetricks.
struct RunnerTool {
  std::string id;
  std::string label;
  std::string doc;
  std::string path;
  bool installed = false;
};

struct RunnerToolsResult {
  bool ok = false;
  ApiError error;
  std::vector<RunnerTool> tools;
};

// GET /v1/runners/{kind}/schema: one key a runner's runner_config takes.
struct RunnerOption {
  std::string key;
  std::string label;
  std::string doc;
};

struct RunnerSchemaResult {
  bool ok = false;
  ApiError error;
  std::vector<RunnerOption> options;
};

// POST /v1/runners/download returns 202 immediately and reports progress on
// the event stream, so "ok" here only means the download started.
struct RunnerDownloadResult {
  bool ok = false;
  ApiError error;
};

// A `runners.download.started` / `.finished` / `.failed` payload.
struct RunnerDownloadEvent {
  std::string kind;
  std::string tag;
  std::string name;
  std::string label;
  std::string source;
  std::string replaced;  // on "finished" after an update: the "kind:name" it replaced
  std::string state;  // "started" | "progress" | "finished" | "failed"
  double progress = -1;  // 0..1, only on "progress"
  ApiError error;     // only on "failed"
};

// POST /v1/steam/scan.
struct SteamScanResult {
  bool ok = false;
  ApiError error;
  int added = 0;
  int updated = 0;
};

// POST /v1/lutris/import.
struct LutrisImportResult {
  bool ok = false;
  ApiError error;
  int added = 0;
  int updated = 0;
  int other_runner = 0;  // left to a runner Mira doesn't drive (steam, dosbox, ...)
  int incomplete = 0;    // wine/linux games whose config can't be imported as-is
};

// POST /v1/games/{id}/run: an arbitrary executable inside this game's own
// prefix, tracked like a normal launch.
struct RunInPrefixResult {
  bool ok = false;
  ApiError error;
};

// POST /v1/games/{id}/finish-install.
struct FinishInstallResult {
  bool ok = false;
  ApiError error;
};

// The frontend's own preferences, which live in frontend.toml, the sibling
// file the backend stores verbatim and never interprets
// (docs/architecture.md), reachable as the opaque `frontend` key of
// GET/PATCH /v1/config.
//
// Deliberately not settings.toml: every key there has to be declared in
// src/config/Schema.cpp and means something to the daemon, whereas none of
// this does.
//
// Every field is optional because the file is allowed to be absent, partial
// or hand-edited: an unset field means "use the built-in default", not
// zero.
struct FrontendPrefs {
  // The unmaximized size, so un-maximizing after a restart has a size to go back to.
  std::optional<int> window_width;
  std::optional<int> window_height;
  std::optional<bool> window_maximized;
  std::optional<int> tile_width;  // the library grid's
  std::optional<std::string> library_filter;  // a filter key
  std::optional<int> sidebar_width;
  std::optional<std::string> sort_by;  // "name" | "last_played" | "playtime" | "status"
  std::optional<bool> sort_descending;
  // Whether opening the frontend also kicks off POST /v1/library/scan.
  // Worth turning off for a large library on slow storage, where the scan
  // is the slowest thing about startup and the daemon's own watcher
  // (library::Watcher) already keeps the library current while it runs.
  std::optional<bool> scan_on_startup;
  // A theme name (ui/Theme.h), or "auto" (the default) to follow the
  // desktop's own light/dark preference.
  std::optional<std::string> theme;
  // On (default): dragging across the grid rubber-band selects tiles.
  std::optional<bool> drag_select;
  // On (default): double-clicking a game in a grid plays it, or stops it while it runs.
  std::optional<bool> double_click_play;
  // Shape adjustments layered over whatever the theme sets, in pixels; see
  // theme::Overrides. Unset means "leave it to the theme".
  std::optional<int> tile_spacing;
  std::optional<int> grid_margin;
  std::optional<int> tile_radius;
  std::optional<int> panel_radius;
  std::optional<int> control_radius;
  // Overridden keyboard shortcuts, id (ui/KeyBindings.h) -> a
  // QKeySequence::toString(PortableText) string. An id absent here just
  // means "whatever that action's own default is" -- see keybindings::All().
  std::optional<std::map<std::string, std::string>> shortcut_overrides;
  // Source ids unticked under "In sidebar" in Manage sources.
  std::optional<std::vector<std::string>> hidden_sources;
  // The sidebar's source order, by id; sources missing from it follow.
  std::optional<std::vector<std::string>> source_order;
  // When each source last imported, as unix seconds.
  std::optional<std::map<std::string, std::int64_t>> source_imported_at;
  // How many recently played games the sidebar lists besides running ones
  // (0, the default, hides them), and whether source rows show a game count.
  std::optional<int> sidebar_recent_count;
  std::optional<bool> sidebar_source_counts;
  // Source rows show a colored tile with the source's initial, not a dot.
  std::optional<bool> sidebar_source_icons;
  // How PINNED and RECENTLY PLAYED draw their games: "covers" (the default),
  // "hero" or "shelf" (see ui/SidebarGames), and whether recent rows say
  // when each was played.
  std::optional<std::string> sidebar_pinned_style;
  std::optional<std::string> sidebar_recent_style;
  std::optional<bool> sidebar_recent_when;
  // The library's filter tabs, and the "Continue playing" cards above the
  // grid (running and recently played games) with how many it shows.
  std::optional<bool> library_filter_tabs;
  std::optional<bool> library_continue_row;
  std::optional<int> library_continue_count;
  std::optional<bool> library_continue_apps;  // apps in the Continue row; off by default
  std::optional<bool> library_apps_in_all;    // apps under the All tab; on by default
  // What a tile draws over its cover besides the title.
  std::optional<bool> tile_status;
  std::optional<bool> tile_source_mark;
  std::optional<bool> tile_pin_badge;
  // Source pages split installed and not installed games into tabs; off
  // stacks both sections.
  std::optional<bool> source_page_tabs;
  // Set once the first-run wizard has been through (or skipped).
  std::optional<bool> onboarded;
  // What Mira is mostly for: "games", "apps" or "both" (the default). Tunes
  // what the sidebar and library lead with; nothing is hidden for good.
  std::optional<std::string> primary_use;
  // Each source page's tile width, by source id; tile_width is the library's.
  // Synced: every page uses tile_width.
  std::optional<std::map<std::string, int>> source_tile_widths;
  std::optional<bool> tile_size_synced;
  // Keys to delete from frontend.toml on save (sent as null), so they read
  // as unset again, e.g. a shape override going back to the theme's.
  std::vector<std::string> clear;
};

struct FrontendPrefsResult {
  bool ok = false;
  ApiError error;
  FrontendPrefs prefs;
};

// GET /v1/games/{id}/installer[?path=].
struct InstallerInfoResult {
  bool ok = false;
  ApiError error;
  std::string path;
  std::int64_t size_bytes = 0;
  std::string format;  // "inno" | "nsis" | "msi" | "unknown"
  bool silent = false;  // Mira knows how to run it without its window
};

// GET /v1/games/{id}/install/progress.
struct InstallProgressResult {
  bool ok = false;
  ApiError error;
  std::string state;  // "idle" | "queued" | "running" | "finished" | "failed"
  std::int64_t bytes_written = 0;
};

// A `game.install.started` / `.finished` / `.failed` payload.
struct InstallEvent {
  std::string id;
  std::string state;
  ApiError error;  // only on "failed"
};

// POST /v1/games/{id}/relocate, /v1/games/{id}/install, and the like:
// success only means mirad accepted or finished it.
struct GameActionResult {
  bool ok = false;
  ApiError error;
};

// GET /v1/games/{id}/metadata/matches: SteamGridDB games whose art could
// be this one's, best first.
struct GriddbMatch {
  std::int64_t id = 0;
  std::string name;
  int year = 0;  // 0 when SteamGridDB has no release date
};

struct GriddbMatchesResult {
  bool ok = false;
  ApiError error;
  std::string query;
  std::int64_t chosen = 0;  // 0: the top match, nothing chosen
  std::vector<GriddbMatch> matches;
};

// POST /v1/library/relocate.
// One game's failure inside a batch reply.
struct GameFailure {
  std::string id;
  ApiError error;
};

struct RelocateLibraryResult {
  bool ok = false;
  ApiError error;
  int moved = 0;
  int failed = 0;
  std::vector<GameFailure> errors;
};

// POST /v1/games/delete.
struct DeleteGamesResult {
  bool ok = false;
  ApiError error;
  std::vector<std::string> removed;
  std::vector<GameFailure> failed;
};

// GET /v1/games/{id}/log?lines=: the game's own log tail. An empty
// `lines` means nothing was ever logged, not an error.
struct GameLogResult {
  bool ok = false;
  ApiError error;
  std::vector<std::string> lines;
};

// GET /v1/logs/<channel>. `next` is the cursor for the following read; `active` is whether the task is still going.
struct LogResult {
  bool ok = false;
  ApiError error;
  std::vector<std::string> lines;
  std::uint64_t next = 0;
  bool active = false;
  std::string live;  // a line still being redrawn (a progress bar); not in `lines` until it ends
};

// GET /v1/gamemode/status: is Feral GameMode's daemon installed/reachable.
// Purely informational; `launch.gamemode` (a plain config key) is the toggle.
struct GameModeStatusResult {
  bool ok = false;
  ApiError error;
  bool installed = false;
  bool daemon_running = false;
};

// POST /v1/games/{id}/tricks: 202, so this only means "accepted". The
// outcome arrives as a tricks.started/.finished/.failed event.
struct TricksResult {
  bool ok = false;
  ApiError error;
};

// A tricks.started / .finished / .failed payload.
struct TricksEvent {
  std::string id;
  std::string verb;
  std::string state;  // "started" | "finished" | "failed"
  ApiError error;     // only on "failed"
};

// DELETE /v1/runners/{kind}:{name}: synchronous, 200 on success.
struct RunnerRemoveResult {
  bool ok = false;
  ApiError error;
};

// GET /v1/desktop-entries/candidates: an already-installed .desktop entry
// (Flatpak or otherwise) that could become a game. `icon` is a theme icon
// name/path, not image bytes.
struct DesktopEntryCandidate {
  std::string id;
  std::string name;
  std::string icon;
};

struct DesktopEntryCandidatesResult {
  bool ok = false;
  ApiError error;
  std::vector<DesktopEntryCandidate> candidates;
};

// POST /v1/desktop-entries/import.
struct DesktopEntryImportResult {
  bool ok = false;
  ApiError error;
  int added = 0;
  int updated = 0;
};

// POST /v1/desktop-entries/sync: regenerates Mira's own desktop entries.
struct DesktopEntrySyncResult {
  bool ok = false;
  ApiError error;
};

// GET /v1/stores/{id}/status. `tool` is
// the helper mirad drives for that store (Legendary, gogdl, butler,
// humble-cli).
struct StoreStatusResult {
  bool ok = false;
  ApiError error;
  bool tool_installed = false;
  std::string tool_version;
  bool authenticated = false;
  std::string account;  // Epic only
};

// Setup, sign-in, sign-out, and the detached install/download kick-offs:
// success only means mirad accepted it.
struct StoreActionResult {
  bool ok = false;
  ApiError error;
};

// POST /v1/stores/humble/download, once its job has ended.
struct HumbleDownloadResult {
  bool ok = false;
  ApiError error;
  std::string path;
};

// POST /v1/stores/{id}/import.
struct StoreImportResult {
  bool ok = false;
  ApiError error;
  int added = 0;
  int updated = 0;
};

// One GET /v1/library entry: something the account owns.
struct StoreTitle {
  std::string ref;
  std::string title;
  bool installed = false;
  bool owned = true;  // false: listed from an itch collection, but paid and not bought
  std::string source;  // the store it's from
  std::string protondb_tier;  // empty when none is cached
  std::vector<std::string> steam_tags;  // most voted first, empty when none are cached
};

// GET /v1/sources/{id}/removal.
struct RemovalPlanResult {
  bool ok = false;
  ApiError error;
  struct Game {
    std::string id;
    std::string name;
    std::string deletes;  // empty: nothing on disk
  };
  std::vector<Game> games;
  std::string launcher_dir;
  std::vector<std::string> kept;
  bool signs_out = false;
};

// POST /v1/sources/{id}/remove.
struct RemoveSourceResult {
  bool ok = false;
  ApiError error;
  int removed = 0;
  std::vector<std::string> problems;
};

// GET/POST /v1/sources/{id}/runner.
struct SourceRunnerResult {
  bool ok = false;
  ApiError error;
  std::string runner_ref;  // empty: the default runner
  int games = 0;           // the source's Windows games
  int differing = 0;       // of those, the ones on another runner
};

// GET /v1/itch/collections.
struct ItchCollection {
  std::int64_t id = 0;
  std::string title;
  std::int64_t games_count = 0;
  bool own = false;  // the account's own, not added by link
};

struct ItchCollectionsResult {
  bool ok = false;
  ApiError error;
  std::vector<ItchCollection> collections;
};

struct StoreLibraryResult {
  bool ok = false;
  ApiError error;
  std::vector<StoreTitle> titles;
};

struct HumbleBundle {
  std::string key;
  std::string name;
  bool claimed = false;
};

struct HumbleLibraryResult {
  bool ok = false;
  ApiError error;
  std::vector<HumbleBundle> bundles;
};

// GET /v1/launchers: Battle.net, Ubisoft Connect, the EA app.
struct LauncherInfo {
  std::string id;  // also the `source` of the games imported through it
  std::string name;
  std::string game_id;  // the launcher's own game record
  bool installed = false;
  std::string install_state;  // "idle" | "running" | "finished" | "failed"
  bool interactive_install = false;
  std::string prefix;
  std::string runner_ref;
  ApiError error;
};

struct LaunchersResult {
  bool ok = false;
  ApiError error;
  std::vector<LauncherInfo> launchers;
};

// POST /v1/stores/{id}/login/begin: the login page to open.
struct LoginUrlResult {
  bool ok = false;
  ApiError error;
  std::string url;
};

// A store helper's setup, a library install/update, a Humble download or a
// launcher install moving along, from the event stream.
struct StoreEvent {
  std::string source;  // "epic" | "gog" | "itch" | "humble" | "amazon" | "steam" | a launcher id
  std::string kind;    // "setup" | "install" | "download"
  std::string state;   // "started" | "finished" | "failed"
  std::string ref;     // install: the title's ref; download: the bundle key
  ApiError error;      // only on "failed"
  bool update = false;  // install: an update rather than a first install
  double progress = -1;  // install "progress": 0..1
  std::int64_t eta_seconds = -1;  // install "progress", when reported
  double bytes_per_second = -1;
};

}  // namespace mira_gui
