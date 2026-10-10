#include "api/Routes.h"

#include <algorithm>
#include <filesystem>
#include <charconv>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <set>

#include <httplib.h>

#include "api/Http.h"
#include "api/Services.h"
#include "library/GamePatch.h"
#include "config/Resolver.h"
#include "core/Strings.h"
#include "library/FolderTags.h"
#include "library/PrefixNaming.h"
#include "proc/Session.h"
#include "runner/RunnerRegistry.h"

namespace mira::api {
namespace {
using httplib::Request;
using httplib::Response;
using nlohmann::json;

}  // namespace

void RegisterGameRoutes(httplib::Server& http, Services& s) {
  // --- games ----------------------------------------------------------------

  // Leaves out games tagged "hidden" unless a tag is asked for or include_hidden=true,
  // and games from a source that's off unless include_off=true.
  http.Get("/v1/games", [&s](const Request& req, Response& res) {
    std::vector<model::Game> all = s.games.All();
    json out = json::array();
    const auto status_filter = req.params.find("status");
    const auto tag_filter = req.params.find("tag");
    const bool include_hidden = BoolParam(req, "include_hidden");
    const bool include_off = BoolParam(req, "include_off");
    std::set<std::string> off_sources;
    if (!include_off) {
      for (const store::SourceState& source : s.games.Sources()) {
        if (!source.enabled) off_sources.insert(source.id);
      }
    }
    const Services::RecordSettings settings = s.CurrentRecordSettings();
    for (const model::Game& game : all) {
      if (!include_off && off_sources.contains(store::SourceIdOf(game.source))) continue;
      if (status_filter != req.params.end() &&
          status_filter->second != model::ToString(game.status)) {
        continue;
      }
      if (tag_filter != req.params.end()) {
        if (!std::ranges::contains(game.tags, tag_filter->second)) continue;
      } else if (!include_hidden && std::ranges::contains(game.tags, std::string("hidden"))) {
        continue;
      }
      out.push_back(s.Record(game, &settings));
    }
    SendJson(res, std::move(out));
  });

  http.Get(R"(/v1/games/([^/]+))", [&s](const Request& req, Response& res) {
    auto game = s.games.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    json body = s.Record(*game);
    // What an empty runner_ref runs with, so an editor can show that runner's options.
    model::Game unpinned = *game;
    unpinned.runner_ref.clear();
    body["default_runner"] = runner::RunnerRegistry(s.config).ResolveRef(unpinned);
    SendJson(res, body);
  });

  // The game's finished sessions, newest first.
  http.Get(R"(/v1/games/([^/]+)/sessions)", [&s](const Request& req, Response& res) {
    auto game = s.games.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    int limit = 50;
    if (auto it = req.params.find("limit"); it != req.params.end()) {
      const std::string& raw = it->second;
      if (std::from_chars(raw.data(), raw.data() + raw.size(), limit).ec != std::errc() || limit < 1) {
        return SendError(res, 400, "invalid_param", "?limit= must be a whole number, 1 or more");
      }
    }
    json sessions = json::array();
    for (const store::PlaySession& session : s.games.Sessions(game->id, limit)) {
      sessions.push_back({{"started_at", session.started_at},
                          {"ended_at", session.ended_at},
                          {"duration_seconds", session.duration_seconds},
                          {"exit_code", session.exit_code},
                          {"signal", session.signal},
                          {"incomplete", session.incomplete}});
    }
    SendJson(res, {{"sessions", sessions}});
  });

  // The tail of mira-run's log for this game. No log yet is an empty list.
  http.Get(R"(/v1/games/([^/]+)/log)", [&s](const Request& req, Response& res) {
    auto game = s.games.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");

    int requested_lines = 200;
    if (auto it = req.params.find("lines"); it != req.params.end()) {
      const std::string& raw = it->second;
      if (std::from_chars(raw.data(), raw.data() + raw.size(), requested_lines).ec != std::errc() || requested_lines < 1) {
        return SendError(res, 400, "invalid_param", "?lines= must be a whole number, 1 or more");
      }
    }

    const std::filesystem::path log_file = proc::GameLogPath(s.games.Dir(), game->id);
    std::ifstream in(log_file, std::ios::binary);
    if (!in) return SendJson(res, {{"lines", json::array()}});

    // Only the end of the file is read; logs can be up to launch.log_max_mb.
    constexpr std::streamoff kMaxTailBytes = 4 * 1024 * 1024;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(size > kMaxTailBytes ? size - kMaxTailBytes : 0);
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    std::vector<std::string> all_lines = strings::Split(content, '\n');
    if (!all_lines.empty() && all_lines.back().empty()) all_lines.pop_back();  // trailing newline
    const std::size_t take = std::min(all_lines.size(), static_cast<std::size_t>(requested_lines));
    json out = json::array();
    for (std::size_t i = all_lines.size() - take; i < all_lines.size(); ++i) out.push_back(all_lines[i]);
    SendJson(res, {{"lines", out}});
  });

  http.Patch(R"(/v1/games/([^/]+))", [&s](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    const auto body = BodyObject(req, res, "a JSON object");
    if (!body) return;
    const json& b = *body;
    if (const auto problem = library::GamePatchProblem(b)) return SendError(res, 400, "invalid_body", *problem);

    const auto before = s.games.Find(id);
    // Turned into a Windows game without a prefix folder: it gets one, as POST /v1/games/manual
    // does. A game turned native keeps its data_dir, so switching back reuses the same prefix.
    std::string prefix_dir;
    if (before && before->platform != model::Platform::Windows && before->data_dir.empty()) {
      const model::Game patched = library::ParseGamePatch(*before, b);
      if (patched.platform == model::Platform::Windows && patched.data_dir.empty()) {
        prefix_dir = library::PrefixDir(s.config, s.games, patched).string();
      }
    }
    // Turned into a ready Windows game: its prefix is set up in the background, as POST
    // /v1/games/manual does, and it is setting_up until then.
    const bool turned_windows = before && before->platform != model::Platform::Windows;
    bool provision = false;
    auto result = s.games.Update(id, [&](model::Game& game) {
      game = library::ParseGamePatch(game, b);
      if (game.platform == model::Platform::Windows && game.data_dir.empty())
        game.data_dir = prefix_dir;
      provision = turned_windows && game.platform == model::Platform::Windows &&
                  game.status == model::GameStatus::Ready;
      if (provision) game.status = model::GameStatus::SettingUp;
    });
    if (!result) return SendStoreError(res, result.error());
    s.SyncDesktopEntry(id);
    SendJson(res, s.events.Publish("game.updated", s.Record(*result)).payload);
    if (provision) s.ProvisionLater(*result);
    // A new folder too: a game sorted by a link needs its link pointed at it.
    if (before &&
        (before->tags != result->tags || before->folder_tag != result->folder_tag ||
         before->install_path != result->install_path || before->data_dir != result->data_dir))
      s.SortByTags({id});
  });

  // One save, one menu sync and one event for any number of games, so a
  // multi-select doesn't cost a request (and a full rewrite) per game.
  http.Patch("/v1/games", [&s](const Request& req, Response& res) {
    constexpr std::string_view kShape =
        R"({"ids": [...], "add_tags"?: [...], "remove_tags"?: [...], "folder_tag"?: "...", "config"?: {...}})";
    const auto body = BodyObject(req, res, kShape);
    if (!body) return;
    const json& b = *body;
    const auto ids = StringList(b, "ids");
    const auto add_tags = StringList(b, "add_tags");
    const auto remove_tags = StringList(b, "remove_tags");
    const json config = b.value("config", json::object());
    const json folder_tag = b.value("folder_tag", json());
    if (!ids || !add_tags || !remove_tags || !config.is_object() ||
        !(folder_tag.is_null() || folder_tag.is_string())) {
      return SendError(res, 400, "invalid_body", std::format("expected {}", kShape));
    }
    if (auto problem = library::ValidateOverridesPatch(config)) return SendError(res, 400, "invalid_setting", *problem);

    auto updated = s.games.UpdateMany(*ids, [&](model::Game& game) {
      const std::vector<std::string> old_tags = game.tags;
      const std::string old_pick = game.folder_tag;
      const json old_overrides = game.overrides;
      std::erase_if(game.tags, [&](const std::string& tag) { return std::ranges::contains(*remove_tags, tag); });
      for (const std::string& tag : *add_tags) {
        if (!std::ranges::contains(game.tags, tag)) game.tags.push_back(tag);
      }
      // The game's folder from now on, whatever tags.folders' order says; "" goes back to it.
      if (folder_tag.is_string()) {
        const std::string pick = folder_tag.get<std::string>();
        if (!pick.empty() && !std::ranges::contains(game.tags, pick)) game.tags.push_back(pick);
        game.folder_tag = pick;
      }
      library::DropStalePick(game);
      library::ApplyOverridesPatch(game, config);
      return game.tags != old_tags || game.folder_tag != old_pick ||
             game.overrides != old_overrides;
    });
    if (!updated) return SendStoreError(res, updated.error());

    json games = json::array();
    const Services::RecordSettings settings = s.CurrentRecordSettings();
    for (const model::Game& game : *updated) games.push_back(s.Record(game, &settings));
    if (!updated->empty()) {
      // Tags never change a menu entry; only an override can.
      if (!config.empty()) {
        for (const model::Game& game : *updated) s.SyncDesktopEntry(game.id);
      }
      s.events.Publish("games.updated", {{"games", games}});
    }
    SendJson(res, {{"games", std::move(games)}});
    std::vector<std::string> changed;
    for (const model::Game& game : *updated) changed.push_back(game.id);
    s.SortByTags(std::move(changed));
  });

  http.Get(R"(/v1/games/([^/]+)/config)", [&s](const Request& req, Response& res) {
    auto game = s.games.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    config::Resolver resolver(s.config, game->overrides);
    SendJson(res, resolver.EffectiveDocument());
  });

  http.Patch(R"(/v1/games/([^/]+)/config)", [&s](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    const auto body = BodyObject(req, res, "a flat {\"dotted.key\": value} object");
    if (!body) return;
    if (auto problem = library::ValidateOverridesPatch(*body)) {
      return SendError(res, 400, "invalid_setting", *problem);
    }
    auto result =
        s.games.Update(id, [&](model::Game& game) { library::ApplyOverridesPatch(game, *body); });
    if (!result) return SendStoreError(res, result.error());
    // An override can turn desktop_entries.enabled off for this game.
    s.SyncDesktopEntry(id);
    SendJson(res, s.events.Publish("game.updated", s.Record(*result)).payload);
  });

  http.Delete(R"(/v1/games/([^/]+))", [&s](const Request& req, Response& res) {
    auto game = s.games.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");

    const bool purge = BoolParam(req, "purge");
    const auto flag = [&](const char* name) {
      return purge || BoolParam(req, name);
    };
    // Deleting files or a prefix can take minutes (a store's uninstaller, a big folder), so it's
    // a job rather than a request holding one of the server's threads.
    if (flag("delete_files") || flag("delete_prefix")) {
      const std::string id = game->id;
      const bool metadata = flag("delete_metadata");
      return s.StartJob(
          req, res, "delete", id, "Removing " + game->name,
          [&s, id, files = flag("delete_files"), prefix = flag("delete_prefix"),
           metadata](JobRegistry::Progress&) -> Result<json> {
            auto folders_lock = s.games.LockFolders();
            const auto current = s.games.Find(id);
            if (!current) return Err("game_not_found", "the game was removed");
            const auto claim = s.Claim(id, "deleted");  // no launch from files being deleted
            if (!claim) return std::unexpected(claim.error());
            if (auto deleted = s.DeleteGameData(*current, s.games.All(), files, prefix, metadata);
                !deleted) {
              return std::unexpected(deleted.error());
            }
            if (auto removed = s.games.Remove(id); !removed)
              return std::unexpected(removed.error());
            folders_lock.unlock();
            s.SyncDesktopEntry(id);
            s.events.Publish("game.removed", {{"id", id}});
            return json::object();
          });
    }
    const auto folders_lock = s.games.LockFolders();
    const auto claim = s.Claim(game->id, "deleted");
    if (!claim) return SendError(res, 409, claim.error());
    if (auto deleted = s.DeleteGameData(*game, s.games.All(), flag("delete_files"),
                                        flag("delete_prefix"), flag("delete_metadata"));
        !deleted) {
      return SendError(res, 400, deleted.error());
    }

    auto result = s.games.Remove(req.matches[1]);
    if (!result) return SendStoreError(res, result.error());
    s.SyncDesktopEntry(req.matches[1]);
    s.events.Publish("game.removed", {{"id", req.matches[1].str()}});
    SendJson(res, json::object());
  });

  // DELETE /v1/games/{id} for many games, with one save, menu sync and event.
  // A game whose files can't be deleted stays in the library.
  http.Post("/v1/games/delete", [&s](const Request& req, Response& res) {
    constexpr std::string_view kShape = R"({"ids": [...], "delete_files"?, "delete_prefix"?, "delete_metadata"?})";
    const auto body = BodyObject(req, res, kShape);
    if (!body) return;
    const auto ids = StringList(*body, "ids");
    if (!ids) {
      return SendError(res, 400, "invalid_body",
                       std::format("expected {}", kShape));
    }
    const bool purge = body->value("purge", false);
    const bool files = purge || body->value("delete_files", false);
    const bool prefix = purge || body->value("delete_prefix", false);
    const bool metadata = purge || body->value("delete_metadata", false);

    s.StartJob(req, res, "delete", "", "Removing games", [&s, ids = *ids, files, prefix, metadata](
                                                            JobRegistry::Progress& progress) -> Result<json> {
      std::vector<std::string> deletable;
      json failed = json::array();
      auto folders_lock = s.games.LockFolders();
      // Every game claimed first, so none can launch mid-delete. Games that share a folder and are
      // removed together don't keep it from each other; one that couldn't be claimed (running,
      // being moved) stays, and keeps its folder.
      std::map<std::string, proc::ProcessSupervisor::Reservation> claims;
      for (const std::string& id : ids) {
        if (!s.games.Find(id) || claims.contains(id)) continue;
        if (auto claim = s.Claim(id, "deleted")) {
          claims.emplace(id, std::move(*claim));
        } else {
          failed.push_back(BatchFailure(id, claim.error()));
        }
      }
      std::vector<model::Game> staying = s.games.All();
      std::erase_if(staying, [&](const model::Game& game) { return claims.contains(game.id); });
      int done = 0;
      for (const std::string& id : ids) {
        progress.Report(done++, static_cast<int>(ids.size()));
        if (!claims.contains(id)) continue;
        const auto game = s.games.Find(id);
        if (!game) continue;
        if (auto deleted = s.DeleteGameData(*game, staying, files, prefix, metadata); !deleted) {
          failed.push_back(BatchFailure(id, deleted.error()));
          continue;
        }
        deletable.push_back(id);
      }
      auto removed = s.games.RemoveMany(deletable);
      folders_lock.unlock();
      if (!removed) return std::unexpected(removed.error());
      if (!removed->empty()) {
        for (const std::string& removed_id : *removed) s.SyncDesktopEntry(removed_id);
        s.events.Publish("games.removed", {{"ids", *removed}});
      }
      return json{{"removed", *removed}, {"failed", std::move(failed)}};
    });
  });

  // --- manual add -----------------------------------------------------------

  http.Post("/v1/games/manual", [&s](const Request& req, Response& res) {
    constexpr std::string_view kShape = R"({"install_path": "...", "exe_path": "...", "name"?, "platform"?, "is_installer"?})";
    const auto body = BodyObject(req, res, kShape);
    if (!body) return;
    const json& b = *body;
    if (!b.contains("install_path") || !b["install_path"].is_string() ||
        !b.contains("exe_path") || !b["exe_path"].is_string()) {
      return SendError(res, 400, "invalid_body",
                       std::format("expected {}", kShape));
    }
    const std::filesystem::path install_path = b["install_path"].get<std::string>();
    const std::string exe_path = library::StoredExePath(install_path.string(), b["exe_path"]);
    const bool is_installer = b.value("is_installer", false);

    model::Platform platform;
    if (b.contains("platform") && !library::IsSettablePlatform(b["platform"])) {
      return SendError(res, 400, "invalid_body", R"("platform" must be "windows" or "native")");
    }
    if (b.contains("platform")) {
      platform = model::PlatformFromString(b["platform"].get<std::string>());
    } else {
      const std::string ext = strings::ToLower(std::filesystem::path(exe_path).extension().string());
      const bool windows = ext == ".exe" || ext == ".msi" || ext == ".bat" || ext == ".cmd";
      platform = windows ? model::Platform::Windows : model::Platform::Native;
    }

    const auto is_appimage = [](const std::string& exe) {
      return strings::ToLower(std::filesystem::path(exe).extension().string()) == ".appimage";
    };
    const bool appimage = is_appimage(exe_path);
    // The same program again updates its game, and so does another program in a folder that is
    // one game's. An AppImage is a game of its own (two in ~/Applications are two games).
    std::optional<model::Game> existing;
    std::optional<model::Game> folder_game;
    for (const model::Game& known : s.games.All()) {
      if (known.install_path != install_path.string()) continue;
      if (known.exe_path == exe_path) {
        existing = known;
        break;
      }
      if (!appimage && !is_appimage(known.exe_path) && !folder_game) folder_game = known;
    }
    if (!existing) existing = folder_game;
    model::Game game;
    if (existing) game = *existing;
    // An AppImage is the program itself, so it names the game rather than the folder it sits in.
    const std::string default_name = appimage ? std::filesystem::path(exe_path).stem().string()
                                              : install_path.filename().string();
    game.name = b.value("name", strings::CleanGameName(default_name));
    game.id = existing ? game.id : s.games.NextId(game.name);
    game.source = "manual";
    game.install_path = install_path.string();
    game.exe_path = exe_path;
    game.args = b.value("args", std::string());
    game.platform = platform;
    game.updated_at = model::NowSeconds();
    if (!existing) game.created_at = game.updated_at;
    // An installer needs the prefix too: it installs the game into it.
    if (platform != model::Platform::Windows) {
      game.data_dir.clear();
    } else if (game.data_dir.empty()) {
      game.data_dir = library::PrefixDir(s.config, s.games, game).string();
    }

    if (is_installer) {
      // An installer isn't the game, so it isn't provisioned or launchable yet.
      game.status = model::GameStatus::NeedsInstall;
      game.last_error = "This is an installer, not the game itself. Install it to play.";
    } else {
      // A Windows game's prefix is set up in the background; it's setting_up until then.
      game.status = platform == model::Platform::Windows ? model::GameStatus::SettingUp
                                                         : model::GameStatus::Ready;
      game.last_error.clear();
    }

    auto result = s.games.Upsert(game);
    if (!result) return SendError(res, 500, result.error());

    s.SyncDesktopEntry(game.id);
    if (!existing) s.fetches.Enqueue(s.config, s.events, game);
    SendJson(res,
             s.events.Publish(existing ? "game.updated" : "game.added", s.Record(game)).payload);
    if (game.status == model::GameStatus::SettingUp) s.ProvisionLater(game);
  });
}

}  // namespace mira::api
