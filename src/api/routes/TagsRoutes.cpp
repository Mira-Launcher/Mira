#include <httplib.h>

#include <algorithm>
#include <format>
#include <map>

#include "api/Http.h"
#include "api/Routes.h"
#include "api/Services.h"
#include "config/Schema.h"
#include "library/FolderTags.h"
#include "metadata/MetadataFetcher.h"

namespace mira::api {
namespace {
using httplib::Request;
using httplib::Response;
using nlohmann::json;

// The tags Mira gives a meaning to have their own actions; the Tags page leaves them be.
bool IsMeaningTag(const std::string& tag) {
  return library::SameTag(tag, "favorite") || library::SameTag(tag, "hidden") ||
         library::SameTag(tag, "app");
}

// One tag across the library: spelled as first seen, and the games that have it.
struct Tally {
  std::string name;
  std::vector<std::string> ids;
};

void Count(std::vector<Tally>& tallies, const std::string& tag, const std::string& id) {
  auto found =
      std::ranges::find_if(tallies, [&](const Tally& t) { return library::SameTag(t.name, tag); });
  if (found == tallies.end()) {
    tallies.push_back({tag, {}});
    found = tallies.end() - 1;
  }
  if (!std::ranges::contains(found->ids, id)) found->ids.push_back(id);
}

// Most games first, then by name.
void Rank(std::vector<Tally>& tallies) {
  std::ranges::sort(tallies, [](const Tally& a, const Tally& b) {
    if (a.ids.size() != b.ids.size()) return a.ids.size() > b.ids.size();
    return a.name < b.name;
  });
}

// tags.folders with `tag` in or out, keeping the others' order; nullopt when that's no change.
std::optional<std::vector<std::string>> FoldersWith(const config::Config& config,
                                                    const std::string& tag, bool folder) {
  std::vector<std::string> folders = config.GetStringArray("tags.folders");
  const bool listed =
      std::ranges::any_of(folders, [&](const std::string& f) { return library::SameTag(f, tag); });
  if (listed == folder) return std::nullopt;
  if (folder) {
    folders.push_back(tag);
  } else {
    std::erase_if(folders, [&](const std::string& f) { return library::SameTag(f, tag); });
  }
  return folders;
}

// Saves tags.folders the way PATCH /v1/config would; Changed then reacts to it.
Result<void> SaveFolders(Services& s, const std::vector<std::string>& folders) {
  return s.config.Patch({{"tags", {{"folders", folders}}}});
}

// After games' tags changed: one games.updated for them, then their folders follow, unless the
// folder tags changed too, which sorts every game anyway (Services::SettingsChanged).
void Changed(Services& s, const std::vector<model::Game>& updated, bool folders_changed,
             Response& res) {
  json games = json::array();
  std::vector<std::string> ids;
  const Services::RecordSettings settings = s.CurrentRecordSettings();
  for (const model::Game& game : updated) {
    games.push_back(s.Record(game, &settings));
    ids.push_back(game.id);
  }
  if (!updated.empty()) s.events.Publish("games.updated", {{"games", games}});
  SendJson(res, {{"games", std::move(games)}});
  if (folders_changed) {
    s.SettingsChanged({"tags.folders"});
  } else {
    s.SortByTags(std::move(ids));
  }
}

}  // namespace

void RegisterTagsRoutes(httplib::Server& http, Services& s) {
  http.Get("/v1/tags", [&s](const Request&, Response& res) {
    const std::vector<std::string> folders = s.config.GetStringArray("tags.folders");
    std::vector<Tally> mine;
    std::vector<Tally> steam;
    int missing = 0;
    const auto stored = metadata::StoredSteamTags(s.games.Metadata());
    for (const model::Game& game : s.games.All()) {
      for (const std::string& tag : game.tags) {
        if (!IsMeaningTag(tag)) Count(mine, tag, game.id);
      }
      if (game.source == "launcher") continue;  // a store's launcher, not a game
      const auto found = stored.find(game.id);
      if (found == stored.end()) continue;  // no metadata yet: its own fetch brings them
      if (!found->second) {
        if (metadata::WantsSteamTags(s.config, game)) ++missing;
        continue;
      }
      for (const std::string& tag : *found->second) Count(steam, tag, game.id);
    }
    // A folder tag no game has yet is still listed.
    for (const std::string& folder : folders) {
      if (std::ranges::none_of(mine,
                               [&](const Tally& t) { return library::SameTag(t.name, folder); }))
        mine.push_back({folder, {}});
    }
    Rank(mine);
    Rank(steam);

    json tags = json::array();
    for (const Tally& tag : mine) {
      const auto from_steam = std::ranges::find_if(
          steam, [&](const Tally& t) { return library::SameTag(t.name, tag.name); });
      tags.push_back({{"name", tag.name},
                      {"count", tag.ids.size()},
                      {"ids", tag.ids},
                      {"folder", std::ranges::any_of(folders,
                                                     [&](const std::string& f) {
                                                       return library::SameTag(f, tag.name);
                                                     })},
                      {"steam_ids",
                       from_steam != steam.end() ? from_steam->ids : std::vector<std::string>{}}});
    }
    json suggestions = json::array();
    for (const Tally& tag : steam) {
      if (std::ranges::any_of(mine,
                              [&](const Tally& t) { return library::SameTag(t.name, tag.name); }))
        continue;
      suggestions.push_back({{"name", tag.name}, {"count", tag.ids.size()}, {"ids", tag.ids}});
    }
    SendJson(
        res,
        {{"tags", std::move(tags)}, {"steam", std::move(suggestions)}, {"steam_missing", missing}});
  });

  // Steam tags for the games whose metadata predates them (or whose last try got no answer).
  http.Post("/v1/tags/fetch", [&s](const Request& req, Response& res) {
    s.StartJob(req, res, "tags", "", "Getting tags from Steam",
               [&s](JobRegistry::Progress&) -> Result<json> {
                 std::vector<model::Game> wanting;
                 const auto stored = metadata::StoredSteamTags(s.games.Metadata());
                 for (const model::Game& game : s.games.All()) {
                   const auto found = stored.find(game.id);
                   if (found != stored.end() && !found->second && metadata::WantsSteamTags(s.config, game))
                     wanting.push_back(game);
                 }
                 return json{{"fetched", metadata::FetchSteamTags(s.config, s.games.Metadata(), wanting)}};
               });
  });

  // Exactly the games in `ids` have `name` afterwards: it's added where missing (at the end) and
  // taken off the rest. `folder` also makes it a folder tag, or stops it being one.
  http.Post("/v1/tags/set", [&s](const Request& req, Response& res) {
    constexpr std::string_view kShape = R"({"name": "...", "ids": [...], "folder"?: bool})";
    const auto parsed = BodyObject(req, res, kShape);
    if (!parsed) return;
    const json& body = *parsed;
    const auto ids = StringList(body, "ids");
    const json folder = body.value("folder", json());
    if (!ids || !body.contains("name") || !body["name"].is_string() ||
        body["name"].get<std::string>().empty() || !(folder.is_null() || folder.is_boolean())) {
      return SendError(res, 400, "invalid_body", std::format("expected {}", kShape));
    }
    const std::string name = body["name"];
    if (IsMeaningTag(name))
      return SendError(res, 400, "invalid_body", std::format("\"{}\" has its own actions", name));

    const auto folders =
        folder.is_boolean() ? FoldersWith(s.config, name, folder.get<bool>()) : std::nullopt;
    if (folders) {
      if (auto problem = config::Schema::Instance().Validate("tags.folders", *folders))
        return SendError(res, 400, "invalid_setting", *problem);
    }
    std::vector<std::string> touched;
    for (const model::Game& game : s.games.All()) {
      const bool has = std::ranges::any_of(
          game.tags, [&](const std::string& t) { return library::SameTag(t, name); });
      if (has != std::ranges::contains(*ids, game.id)) touched.push_back(game.id);
    }
    auto updated = s.games.UpdateMany(touched, [&](model::Game& game) {
      if (std::ranges::contains(*ids, game.id)) {
        game.tags.push_back(name);
      } else {
        std::erase_if(game.tags, [&](const std::string& t) { return library::SameTag(t, name); });
        library::DropStalePick(game);
      }
      return true;
    });
    if (!updated) return SendStoreError(res, updated.error());
    if (folders) {
      if (auto saved = SaveFolders(s, *folders); !saved) return SendError(res, 500, saved.error());
    }
    Changed(s, *updated, folders.has_value(), res);
  });

  // Every spelling of `from` becomes `to`, in place, on every game and in tags.folders.
  http.Post("/v1/tags/rename", [&s](const Request& req, Response& res) {
    constexpr std::string_view kShape = R"({"from": "...", "to": "..."})";
    const auto parsed = BodyObject(req, res, kShape);
    if (!parsed) return;
    const json& body = *parsed;
    if (!body.contains("from") || !body["from"].is_string() || !body.contains("to") ||
        !body["to"].is_string() || body["to"].get<std::string>().empty()) {
      return SendError(res, 400, "invalid_body", std::format("expected {}", kShape));
    }
    const std::string from = body["from"];
    const std::string to = body["to"];
    if (IsMeaningTag(from) || IsMeaningTag(to)) {
      return SendError(res, 400, "invalid_body", "favorite, hidden and app have their own actions");
    }
    std::vector<std::string> folders = s.config.GetStringArray("tags.folders");
    const bool folder_renamed = std::ranges::any_of(
        folders, [&](const std::string& f) { return library::SameTag(f, from); });
    if (folder_renamed) {
      for (std::string& f : folders) {
        if (library::SameTag(f, from)) f = to;
      }
      if (auto problem = config::Schema::Instance().Validate("tags.folders", folders))
        return SendError(res, 400, "invalid_setting", *problem);
    }
    std::vector<std::string> touched;
    for (const model::Game& game : s.games.All()) {
      if (std::ranges::any_of(game.tags,
                              [&](const std::string& t) { return library::SameTag(t, from); }))
        touched.push_back(game.id);
    }
    auto updated = s.games.UpdateMany(touched, [&](model::Game& game) {
      std::vector<std::string> renamed;
      for (const std::string& tag : game.tags) {
        const std::string now = library::SameTag(tag, from) ? to : tag;
        if (std::ranges::none_of(renamed,
                                 [&](const std::string& t) { return library::SameTag(t, now); }))
          renamed.push_back(now);
      }
      game.tags = std::move(renamed);
      if (library::SameTag(game.folder_tag, from)) game.folder_tag = to;
      return true;
    });
    if (!updated) return SendStoreError(res, updated.error());
    if (folder_renamed) {
      if (auto saved = SaveFolders(s, folders); !saved) return SendError(res, 500, saved.error());
    }
    Changed(s, *updated, folder_renamed, res);
  });

  // Takes `name` off every game, and out of tags.folders.
  http.Post("/v1/tags/remove", [&s](const Request& req, Response& res) {
    const auto parsed = BodyObject(req, res, R"({"name": "..."})");
    if (!parsed) return;
    const json& body = *parsed;
    if (!body.contains("name") || !body["name"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"name": "..."})");
    }
    const std::string name = body["name"];
    if (IsMeaningTag(name))
      return SendError(res, 400, "invalid_body", std::format("\"{}\" has its own actions", name));
    const auto folders = FoldersWith(s.config, name, false);
    std::vector<std::string> touched;
    for (const model::Game& game : s.games.All()) {
      if (std::ranges::any_of(game.tags,
                              [&](const std::string& t) { return library::SameTag(t, name); }))
        touched.push_back(game.id);
    }
    auto updated = s.games.UpdateMany(touched, [&](model::Game& game) {
      std::erase_if(game.tags, [&](const std::string& t) { return library::SameTag(t, name); });
      library::DropStalePick(game);
      return true;
    });
    if (!updated) return SendStoreError(res, updated.error());
    if (folders) {
      if (auto saved = SaveFolders(s, *folders); !saved) return SendError(res, 500, saved.error());
    }
    Changed(s, *updated, folders.has_value(), res);
  });

  // Which games would move if tags.folders and tags.sorted_roots were these, and games had these
  // (unsaved) tags, before any of it is saved.
  http.Post("/v1/tags/preview", [&s](const Request& req, Response& res) {
    constexpr std::string_view kShape =
        R"({"folders"?: [...], "sorted_roots"?: [...], "tags"?: {"<id>": [...]}})";
    const auto parsed = BodyObject(req, res, kShape);
    if (!parsed) return;
    const json& body = *parsed;
    const json tags = body.value("tags", json::object());
    const bool tags_valid =
        tags.is_object() && std::ranges::all_of(tags.items(), [](const auto& entry) {
          return entry.value().is_array() &&
                 std::ranges::all_of(entry.value(), [](const json& t) { return t.is_string(); });
        });
    const auto folders = body.contains("folders") ? StringList(body, "folders")
                                                  : std::optional<std::vector<std::string>>{};
    const auto sorted_roots = body.contains("sorted_roots")
                                  ? StringList(body, "sorted_roots")
                                  : std::optional<std::vector<std::string>>{};
    if (!tags_valid || (body.contains("folders") && !folders) ||
        (body.contains("sorted_roots") && !sorted_roots)) {
      return SendError(res, 400, "invalid_body", std::format("expected {}", kShape));
    }
    library::SortRules rules(s.config);
    if (folders) {
      if (auto problem = config::Schema::Instance().Validate("tags.folders", *folders))
        return SendError(res, 400, "invalid_setting", *problem);
      rules.folders = *folders;
    }
    if (sorted_roots) {
      rules.sorted_roots.clear();
      for (const std::string& root : *sorted_roots)
        rules.sorted_roots.push_back(library::NormalRoot(root));
    }
    json moving = json::array();
    for (model::Game game : s.games.All()) {
      if (tags.contains(game.id)) game.tags = tags[game.id].get<std::vector<std::string>>();
      if (!library::NeedsPlacing(rules, game)) continue;
      const auto to = library::SortsByLink(rules, game) ? library::PlacedLinkPath(rules, game)
                                                        : library::PlacedInstallPath(rules, game);
      moving.push_back(
          {{"id", game.id}, {"name", game.name}, {"to", to ? json(to->string()) : json(nullptr)}});
    }
    SendJson(res, {{"moving", std::move(moving)}});
  });
}

}  // namespace mira::api
